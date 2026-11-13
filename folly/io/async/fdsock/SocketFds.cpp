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

#include "SocketFds.h"

namespace folly {

void SocketFds::cloneToSendFromOrDfatal(const SocketFds& other) {
  if (other.empty()) {
    ptr_.reset();
  } else {
    auto* fds = std::get_if<ToSendPair>(other.ptr_.get());
    if (fds == nullptr) [[unlikely]] {
      LOG(DFATAL) << "SocketFds was in 'received' state, not cloning";
      ptr_.reset();
    } else {
      // Cloning is only for "multi-publisher" scenarios.  In these cases
      // we must first clone, and then bind a socket sequence number.
      DCHECK_EQ(kNoSeqNum, fds->second)
          << "Cannot clone SocketFds once it was bound to a socket";
      ptr_ = std::make_unique<FdsVariant>(*fds);
    }
  }
}

SocketFds::Received SocketFds::releaseReceived() {
  auto fds =
      std::move(CHECK_NOTNULL(std::get_if<ReceivedPair>(ptr_.get()))->first);
  // NB: In the case of a Thrift server handler method that is receiving
  // and then sending back FDs using the same `SocketFds` object, this
  // deallocation (and subsequent allocation) could be avoided, e.g. by:
  //  - Without changing the API by having an additional `std::monostate`
  //    representing the variant being empty. This has the downside of
  //    holding on to allocations unnecessarily in other cases.
  //  - By adding `std::pair<Received, ToSend&> releaseReceivedAndSend()`.
  //    This complicates the user experience.
  ptr_.reset();
  return fds;
}

std::optional<SocketFds::ToSendPair> SocketFds::releaseToSendAndSeqNum() {
  auto* fdsPtr = std::get_if<ToSendPair>(ptr_.get());
  if (fdsPtr == nullptr) [[unlikely]] {
    // This can "legitimately" happen if a client wrongly sends FDs to a
    // server method that is not expecting them. Then, `THeader::fds`
    // in a Thrift request-response handler will retain the received
    // FDs by the time a response
    if (ptr_) {
      LOG(WARNING) << "releaseToSendAndSeqNum discarded received FDs";
      ptr_.reset();
    }
    return std::nullopt;
  }
  auto fdsAndSeqNum = std::move(*fdsPtr);
  ptr_.reset();
  return fdsAndSeqNum;
}

void SocketFds::setFdSocketSeqNumOnce(SeqNum seqNum) {
  // The type is unsigned because Thrift IDL only supports signed.
  DCHECK_GE(seqNum, 0) << "Sequence number must be nonnegative";
  if (ptr_ != nullptr) [[likely]] {
    std::visit(
        [seqNum](auto&& v) {
          DCHECK_EQ(kNoSeqNum, v.second) << "Can only set sequence number once";
          v.second = seqNum;
        },
        *ptr_);
  } else {
    LOG(DFATAL) << "Cannot set sequence number on empty SocketFds";
  }
}

SocketFds::SeqNum SocketFds::getFdSocketSeqNum() const {
  if (ptr_ != nullptr) [[likely]] {
    auto seqNum = std::visit([](auto&& v) { return v.second; }, *ptr_);
    if (seqNum >= 0) {
      return seqNum;
    }
    if (seqNum != kNoSeqNum) {
      LOG(DFATAL) << "Sequence number in invalid state: " << seqNum;
    }
  } else {
    LOG(DFATAL) << "Cannot query sequence number of empty SocketFds";
  }
  return kNoSeqNum;
}

SocketFds::SeqNum detail::addSocketFdsSeqNum(
    SocketFds::SeqNum a, SocketFds::SeqNum b) noexcept {
  if (a < 0 || b < 0) {
    LOG(DFATAL) << "Inputs must be nonnegative, got " << a << " + " << b;
    return SocketFds::kNoSeqNum;
  }
  const auto gap = std::numeric_limits<SocketFds::SeqNum>::max() - a;
  if (b <= gap) [[likely]] {
    return a + b;
  }
  return b - gap - 1; // wrap around through 0, modulo max
}

} // namespace folly
