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

#include <folly/io/async/CmsgUtil.h>

#if FOLLY_HAS_LIBURING

#include <cstdint>
#include <cstring>
#include <limits>

#include <folly/CPortability.h>

FOLLY_PUSH_WARNING
FOLLY_CLANG_DISABLE_WARNING("-Wnested-anon-types")
FOLLY_CLANG_DISABLE_WARNING("-Wzero-length-array")
FOLLY_GCC_DISABLE_WARNING("-Wshadow")
#include <liburing.h> // @manual
FOLLY_POP_WARNING

#include <folly/io/IOBuf.h>

namespace folly::detail {

bool parseCmsgs(
    const cmsghdr* firstCmsg,
    size_t controlLength,
    int msgFlags,
    IoUringRecvmsgControl& control) {
  control = {};
  control.msgFlags = msgFlags;
  if (controlLength == 0) {
    return firstCmsg == nullptr;
  }
  if (!firstCmsg || controlLength < sizeof(cmsghdr)) {
    return false;
  }

  const auto* cursor = reinterpret_cast<const uint8_t*>(firstCmsg);
  size_t remaining = controlLength;
  while (remaining >= sizeof(cmsghdr)) {
    const auto* cmsg = reinterpret_cast<const cmsghdr*>(cursor);
    if (cmsg->cmsg_len < CMSG_LEN(0) || cmsg->cmsg_len > remaining) {
      return false;
    }

    const auto payloadLength = cmsg->cmsg_len - CMSG_LEN(0);
    const auto* payload = reinterpret_cast<const uint8_t*>(CMSG_DATA(cmsg));
    control.cmsgs.push_back(
        IoUringCmsg{
            cmsg->cmsg_level,
            cmsg->cmsg_type,
            std::vector<uint8_t>(payload, payload + payloadLength)});

    const auto alignedLength = CMSG_ALIGN(cmsg->cmsg_len);
    if (alignedLength > remaining) {
      break;
    }
    cursor += alignedLength;
    remaining -= alignedLength;
  }
  return true;
}

bool serializeCmsgs(
    const IoUringRecvmsgControl& control,
    void* buffer,
    size_t bufferLength,
    msghdr& msg) {
  msg = {};
  msg.msg_control = buffer;
  msg.msg_flags = control.msgFlags;
  if (bufferLength != 0 && !buffer) {
    return false;
  }

  auto* cursor = static_cast<uint8_t*>(buffer);
  size_t used = 0;
  for (const auto& cmsg : control.cmsgs) {
    if (cmsg.payload.size() >
        std::numeric_limits<size_t>::max() - CMSG_SPACE(0)) {
      return false;
    }
    const auto recordLength = CMSG_SPACE(cmsg.payload.size());
    if (recordLength > bufferLength - used) {
      msg.msg_flags |= MSG_CTRUNC;
      break;
    }

    memset(cursor + used, 0, recordLength);
    cmsghdr header{};
    header.cmsg_len = CMSG_LEN(cmsg.payload.size());
    header.cmsg_level = cmsg.level;
    header.cmsg_type = cmsg.type;
    memcpy(cursor + used, &header, sizeof(header));
    memcpy(
        cursor + used + CMSG_LEN(0), cmsg.payload.data(), cmsg.payload.size());
    used += recordLength;
  }
  msg.msg_controllen = used;
  return true;
}

bool parseIoUringRecvmsg(
    std::unique_ptr<IOBuf>& data,
    int recvmsgLength,
    size_t nameLength,
    size_t controlLength,
    IoUringRecvmsgControl& control) {
  if (!data || recvmsgLength < 0 ||
      nameLength >
          static_cast<size_t>(
              std::numeric_limits<decltype(msghdr::msg_namelen)>::max())) {
    return false;
  }

  const auto chainLength = data->computeChainDataLength();
  if (chainLength != static_cast<size_t>(recvmsgLength) ||
      chainLength > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  if (data->isChained()) {
    data->coalesce();
  }

  msghdr msgh{};
  msgh.msg_namelen = static_cast<decltype(msgh.msg_namelen)>(nameLength);
  msgh.msg_controllen = controlLength;
  void* base = data->writableData();
  auto* out = ::io_uring_recvmsg_validate(base, recvmsgLength, &msgh);
  if (!out || out->namelen > nameLength || out->controllen > controlLength ||
      (out->flags & MSG_TRUNC)) {
    return false;
  }

  auto* payload = ::io_uring_recvmsg_payload(out, &msgh);
  const auto headerBytes = static_cast<size_t>(
      reinterpret_cast<char*>(payload) - reinterpret_cast<char*>(base));
  const auto availablePayloadLength =
      ::io_uring_recvmsg_payload_length(out, recvmsgLength, &msgh);
  if (headerBytes > chainLength || out->payloadlen > availablePayloadLength ||
      out->payloadlen > chainLength - headerBytes) {
    return false;
  }

  if (!parseCmsgs(
          ::io_uring_recvmsg_cmsg_firsthdr(out, &msgh),
          out->controllen,
          out->flags,
          control)) {
    return false;
  }

  data->trimStart(headerBytes);
  data->trimEnd(data->length() - out->payloadlen);
  return true;
}

} // namespace folly::detail

#endif
