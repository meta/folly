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
#include <folly/io/async/Liburing.h>

#if FOLLY_HAS_LIBURING
#include <liburing/io_uring.h>
#endif

#include <array>
#include <cstring>
#include <string_view>
#include <vector>

#include <folly/io/IOBuf.h>
#include <folly/portability/GTest.h>

namespace folly {

#if FOLLY_HAS_LIBURING
namespace {

constexpr size_t kControlLength = 256;
constexpr int kCmsgLevel = 7;
constexpr int kCmsgType = 9;
constexpr std::array<uint8_t, 3> kCmsgPayload{0x17, 0x00, 0x42};

std::vector<uint8_t> makeRecvmsgBlob(
    std::string_view payload,
    const detail::IoUringRecvmsgControl& control,
    size_t reportedPayloadLength = 0) {
  alignas(cmsghdr) std::array<uint8_t, kControlLength> controlBuffer{};
  msghdr msg{};
  CHECK(
      detail::serializeCmsgs(
          control, controlBuffer.data(), controlBuffer.size(), msg));
  CHECK_EQ(control.msgFlags, msg.msg_flags);

  std::vector<uint8_t> blob(
      sizeof(io_uring_recvmsg_out) + kControlLength + payload.size());
  auto* out = reinterpret_cast<io_uring_recvmsg_out*>(blob.data());
  out->namelen = 0;
  out->controllen = static_cast<uint32_t>(msg.msg_controllen);
  out->payloadlen = static_cast<uint32_t>(
      reportedPayloadLength == 0 ? payload.size() : reportedPayloadLength);
  out->flags = msg.msg_flags;

  memcpy(blob.data() + sizeof(*out), controlBuffer.data(), msg.msg_controllen);
  memcpy(
      blob.data() + sizeof(*out) + kControlLength,
      payload.data(),
      payload.size());
  return blob;
}

std::unique_ptr<IOBuf> makeChainedBuffer(const std::vector<uint8_t>& data) {
  const auto splitOffset = sizeof(io_uring_recvmsg_out) / 2;
  auto first = IOBuf::copyBuffer(data.data(), splitOffset);
  first->appendToChain(
      IOBuf::copyBuffer(data.data() + splitOffset, data.size() - splitOffset));
  return first;
}

detail::IoUringRecvmsgControl makeControl(int flags = 0) {
  return detail::IoUringRecvmsgControl{
      {detail::IoUringCmsg{
          kCmsgLevel,
          kCmsgType,
          std::vector<uint8_t>(kCmsgPayload.begin(), kCmsgPayload.end())}},
      flags};
}

bool parseRecvmsgBlob(
    std::unique_ptr<IOBuf>& data, detail::IoUringRecvmsgControl& control) {
  return detail::parseIoUringRecvmsg(
      data,
      static_cast<int>(data->computeChainDataLength()),
      0,
      kControlLength,
      control);
}

TEST(CmsgUtilTest, ParsesFullCmsgPayloadFromChainedBuffer) {
  constexpr std::string_view kPayload = "payload";
  auto blob = makeRecvmsgBlob(kPayload, makeControl());
  auto data = makeChainedBuffer(blob);
  detail::IoUringRecvmsgControl control;

  ASSERT_TRUE(parseRecvmsgBlob(data, control));

  ASSERT_EQ(1, control.cmsgs.size());
  EXPECT_EQ(kCmsgLevel, control.cmsgs[0].level);
  EXPECT_EQ(kCmsgType, control.cmsgs[0].type);
  EXPECT_EQ(
      std::vector<uint8_t>(kCmsgPayload.begin(), kCmsgPayload.end()),
      control.cmsgs[0].payload);
  EXPECT_EQ(0, control.msgFlags);
  EXPECT_EQ(
      kPayload,
      std::string_view(
          reinterpret_cast<const char*>(data->data()), data->length()));
  EXPECT_FALSE(data->isChained());
}

TEST(CmsgUtilTest, ParsesMultipleCmsgsIncludingTimestampPayload) {
  const std::array<timespec, 3> timestamps{
      timespec{1, 2}, timespec{3, 4}, timespec{5, 6}};
  const auto* timestampBytes =
      reinterpret_cast<const uint8_t*>(timestamps.data());
  auto expected = makeControl();
  expected.cmsgs.push_back(
      detail::IoUringCmsg{
          SOL_SOCKET,
          SO_TIMESTAMPING,
          std::vector<uint8_t>(
              timestampBytes, timestampBytes + sizeof(timestamps))});
  auto blob = makeRecvmsgBlob("payload", expected);
  auto data = IOBuf::copyBuffer(blob.data(), blob.size());
  detail::IoUringRecvmsgControl actual;

  ASSERT_TRUE(parseRecvmsgBlob(data, actual));
  ASSERT_EQ(2, actual.cmsgs.size());
  EXPECT_EQ(expected.cmsgs[0].payload, actual.cmsgs[0].payload);
  EXPECT_EQ(SOL_SOCKET, actual.cmsgs[1].level);
  EXPECT_EQ(SO_TIMESTAMPING, actual.cmsgs[1].type);
  EXPECT_EQ(expected.cmsgs[1].payload, actual.cmsgs[1].payload);
}

TEST(CmsgUtilTest, PreservesControlTruncationFlag) {
  auto blob = makeRecvmsgBlob("payload", makeControl(MSG_CTRUNC));
  auto data = IOBuf::copyBuffer(blob.data(), blob.size());
  detail::IoUringRecvmsgControl control;

  ASSERT_TRUE(parseRecvmsgBlob(data, control));
  EXPECT_TRUE(control.msgFlags & MSG_CTRUNC);
  ASSERT_EQ(1, control.cmsgs.size());
  EXPECT_EQ(
      std::vector<uint8_t>(kCmsgPayload.begin(), kCmsgPayload.end()),
      control.cmsgs[0].payload);
}

TEST(CmsgUtilTest, SerializesAllCmsgsAndMarksSmallBufferTruncated) {
  auto control = makeControl();
  control.cmsgs.push_back(
      detail::IoUringCmsg{11, 13, std::vector<uint8_t>{1, 2, 3, 4}});
  alignas(cmsghdr) std::array<uint8_t, CMSG_SPACE(kCmsgPayload.size())>
      buffer{};
  msghdr msg{};

  ASSERT_TRUE(
      detail::serializeCmsgs(control, buffer.data(), buffer.size(), msg));
  EXPECT_TRUE(msg.msg_flags & MSG_CTRUNC);
  ASSERT_NE(nullptr, CMSG_FIRSTHDR(&msg));
  EXPECT_EQ(kCmsgLevel, CMSG_FIRSTHDR(&msg)->cmsg_level);
  EXPECT_EQ(nullptr, CMSG_NXTHDR(&msg, CMSG_FIRSTHDR(&msg)));
}

TEST(CmsgUtilTest, SerializesIntoUnalignedLegacyBuffer) {
  auto control = makeControl();
  std::array<uint8_t, CMSG_SPACE(kCmsgPayload.size()) + 1> storage{};
  msghdr msg{};

  ASSERT_TRUE(
      detail::serializeCmsgs(
          control, storage.data() + 1, storage.size() - 1, msg));
  EXPECT_FALSE(msg.msg_flags & MSG_CTRUNC);
  cmsghdr header{};
  memcpy(&header, storage.data() + 1, sizeof(header));
  EXPECT_EQ(kCmsgLevel, header.cmsg_level);
  EXPECT_EQ(kCmsgType, header.cmsg_type);
  EXPECT_EQ(CMSG_LEN(kCmsgPayload.size()), header.cmsg_len);
}

TEST(CmsgUtilTest, RejectsTruncatedRecvmsgHeader) {
  auto data = IOBuf::copyBuffer("short");
  detail::IoUringRecvmsgControl control;
  EXPECT_FALSE(parseRecvmsgBlob(data, control));
}

TEST(CmsgUtilTest, RejectsOversizedPayload) {
  auto blob = makeRecvmsgBlob("payload", makeControl(), 64);
  auto data = IOBuf::copyBuffer(blob.data(), blob.size());
  detail::IoUringRecvmsgControl control;
  EXPECT_FALSE(parseRecvmsgBlob(data, control));
}

TEST(CmsgUtilTest, RejectsOversizedCmsg) {
  auto blob = makeRecvmsgBlob("payload", makeControl());
  auto* cmsg =
      reinterpret_cast<cmsghdr*>(blob.data() + sizeof(io_uring_recvmsg_out));
  cmsg->cmsg_len = kControlLength + 1;
  auto data = IOBuf::copyBuffer(blob.data(), blob.size());
  detail::IoUringRecvmsgControl control;
  EXPECT_FALSE(parseRecvmsgBlob(data, control));
}

} // namespace
#endif

} // namespace folly
