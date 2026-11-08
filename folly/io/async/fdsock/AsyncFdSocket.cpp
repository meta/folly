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

#include <limits>
#include <fmt/core.h>

#include "AsyncFdSocket.h"

namespace folly {

AsyncFdSocket::AsyncFdSocket(EventBase* evb)
    : AsyncSocket(evb)
#if !defined(_WIN32)
      ,
      readAncillaryDataCob_(this) {
  setUpCallbacks();
}
#else
{
}
#endif

AsyncFdSocket::AsyncFdSocket(
    EventBase* evb,
    const folly::SocketAddress& address,
    uint32_t connectTimeout)
    : AsyncFdSocket(evb) {
  connect(nullptr, address, connectTimeout);
}

AsyncFdSocket::AsyncFdSocket(
    EventBase* evb, NetworkSocket fd, const folly::SocketAddress* peerAddress)
    : AsyncSocket(evb, fd, /* zeroCopyBufId */ 0, peerAddress)
#if !defined(_WIN32)
      ,
      readAncillaryDataCob_(this) {
  setUpCallbacks();
}
#else
{
}
#endif

AsyncFdSocket::AsyncFdSocket(
    AsyncFdSocket::DoesNotMoveFdSocketState, AsyncSocket* sock)
    : AsyncSocket(sock)
#if !defined(_WIN32)
      ,
      readAncillaryDataCob_(this) {
  setUpCallbacks();
}
#else
{
}
#endif

AsyncFdSocket::AsyncFdSocket(
    AsyncFdSocket::DoesNotMoveFdSocketState tag, AsyncSocket::UniquePtr sock)
    : AsyncFdSocket(tag, sock.get()) {}

void AsyncFdSocket::writeChainWithFds(
    WriteCallback* callback,
    std::unique_ptr<folly::IOBuf> buf,
    SocketFds socketFds,
    WriteFlags flags) {
#if defined(_WIN32)
  DCHECK(socketFds.empty()) << "AsyncFdSocket cannot send FDs on Windows";
#else
  DCHECK_EQ(&sendMsgCob_, getSendMsgParamsCB());

  // All these `failWrite` scenarios destroy the FDs and block socket writes.
  if (!socketFds.empty()) {
    if (buf->empty()) {
      DestructorGuard dg(this);
      AsyncSocketException ex(
          AsyncSocketException::BAD_ARGS,
          withAddr("Cannot send FDs without at least 1 data byte"));
      return failWrite(__func__, callback, 0, ex);
    }

    auto maybeFdsAndSeqNum = socketFds.releaseToSendAndSeqNum();
    if (!maybeFdsAndSeqNum) {
      DestructorGuard dg(this);
      AsyncSocketException ex(
          AsyncSocketException::BAD_ARGS,
          withAddr("Cannot send `SocketFds` that is in `Received` state"));
      return failWrite(__func__, callback, 0, ex);
    }
    auto& fds = maybeFdsAndSeqNum->first;
    const auto fdsSeqNum = maybeFdsAndSeqNum->second;

    if (fdsSeqNum == SocketFds::kNoSeqNum) {
      DestructorGuard dg(this);
      AsyncSocketException ex(
          AsyncSocketException::BAD_ARGS,
          withAddr("Sequence number must be set to send FDs"));
      return failWrite(__func__, callback, 0, ex);
    }

    if (fdsSeqNum != sentFdsSeqNum_) {
      DestructorGuard dg(this);
      AsyncSocketException ex(
          AsyncSocketException::BAD_ARGS,
          withAddr(
              fmt::format(
                  "SeqNum of FDs did not match that of socket: {} vs {}",
                  fdsSeqNum,
                  sentFdsSeqNum_)));
      return failWrite(__func__, callback, 0, ex);
    }
    sentFdsSeqNum_ = detail::addSocketFdsSeqNum(sentFdsSeqNum_, fds.size());
    // No DCHECK_GE(allocatedToSendFdsSeqNum_, sentFdsSeqNum_), since it can
    // theoretically happen that "allocated" wraps around before "sent".

    if (!sendMsgCob_.registerFdsForWriteTag(
            WriteRequestTag{buf.get()}, std::move(fds))) {
      // Careful: this has no unittest coverage because I don't have a good
      // idea for how to cause this in a meaningful way.  Should this be
      // a DCHECK? Plans that don't work:
      //  - Creating two `unique_ptr` from the same raw pointer would be
      //    bad news bears for the test.
      //  - IOBuf recycling via getReleaseIOBufCallback() shouldn't be
      //    possible either, since we unregister the tag in our `releaseIOBuf`
      //    override below before releasing the IOBuf.
      DestructorGuard dg(this);
      AsyncSocketException ex(
          AsyncSocketException::BAD_ARGS,
          withAddr("Buffer was already owned by this socket"));
      return failWrite(__func__, callback, 0, ex);
    }
  }

#endif // !Windows

  writeChain(callback, std::move(buf), flags);
}

// The callbacks aren't defined on Windows -- the CMSG macros don't compile
#if !defined(_WIN32)

void AsyncFdSocket::setUpCallbacks() noexcept {
  AsyncSocket::setSendMsgParamCB(&sendMsgCob_);
  AsyncSocket::setReadAncillaryDataCB(&readAncillaryDataCob_);
}

void AsyncFdSocket::swapFdReadStateWith(AsyncFdSocket* other) {
  // We don't need these write-state assertions to correctly swap read
  // state, but since the only use-case is `moveToPlaintext`, they help.
  DCHECK_EQ(0, other->allocatedToSendFdsSeqNum_);
  DCHECK_EQ(0, other->sentFdsSeqNum_);
  DCHECK_EQ(0, other->sendMsgCob_.writeTagToFds_.size());

  fdsQueue_.swap(other->fdsQueue_);
  std::swap(receivedFdsSeqNum_, other->receivedFdsSeqNum_);
  // Do NOT swap `readAncillaryDataCob_` since its internal members are not
  // "state", but plumbing that does not change.
}

void AsyncFdSocket::releaseIOBuf(
    std::unique_ptr<folly::IOBuf> buf, ReleaseIOBufCallback* callback) {
  sendMsgCob_.destroyFdsForWriteTag(WriteRequestTag{buf.get()});
  AsyncSocket::releaseIOBuf(std::move(buf), callback);
}

std::pair<
    size_t,
    AsyncFdSocket::FdSendMsgParamsCallback::WriteTagToFds::iterator>
AsyncFdSocket::FdSendMsgParamsCallback::getCmsgSizeAndFds(
    const AsyncSocket::WriteRequestTag& writeTag) noexcept {
  auto it = writeTagToFds_.find(writeTag);
  if (it == writeTagToFds_.end()) {
    return std::make_pair(0, it);
  }
  return std::make_pair(CMSG_SPACE(sizeof(int) * it->second.size()), it);
}

void AsyncFdSocket::FdSendMsgParamsCallback::getAncillaryData(
    folly::WriteFlags,
    void* data,
    const WriteRequestTag& writeTag,
    const bool /*byteEventsEnabled*/) noexcept {
  auto [cmsgSpace, fdsIt] = getCmsgSizeAndFds(writeTag);
  CHECK_NE(0, cmsgSpace);
  const auto& fds = fdsIt->second;

  // NOT checking `fds.size() < SCM_MAX_FD` here because there's no way to
  // propagate the error to the caller, and ultimately the write will fail
  // out with EINVAL anyway.  The front-end that accepts FDs should check
  // this instead.  If there is a debuggability issue, do add a LOG(ERROR).

  ::msghdr msg{}; // Discarded, we just need to populate `data`

  msg.msg_control = data;
  msg.msg_controllen = cmsgSpace;
  struct ::cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  CHECK_NOTNULL(cmsg);

  cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;

  auto* outFds = reinterpret_cast<int*>(CMSG_DATA(cmsg));
  for (size_t n = 0; n < fds.size(); ++n) {
    outFds[n] = fds[n]->fd();
  }

  VLOG(4) << "this=" << this << ", getAncillaryData() sending " << fds.size()
          << " FDs";
}

uint32_t AsyncFdSocket::FdSendMsgParamsCallback::getAncillaryDataSize(
    folly::WriteFlags,
    const WriteRequestTag& writeTag,
    const bool /*byteEventsEnabled*/) noexcept {
  // Not checking thread because `getAncillaryData` will follow.

  // Due to the unfortunate design of the callback API, this will run
  // again in `getAncillaryData`, let's pray for CPU caches here.
  auto [size, _] = getCmsgSizeAndFds(writeTag);
  return size;
}

void AsyncFdSocket::FdSendMsgParamsCallback::wroteBytes(
    const WriteRequestTag& writeTag) noexcept {
  auto nh = writeTagToFds_.extract(writeTag);
  if (nh.empty()) {
    // `AsyncSocket` will only call `wroteBytes` if `getAncillaryDataSize`
    // returned a nonzero value, so we'll never get here.
    LOG(DFATAL) << "wroteBytes without a matching `getAncillaryData`?";
  } else {
    VLOG(5) << "this=" << this << ", FdSendMsgParamsCallback::wroteBytes() on "
            << nh.mapped().size() << " FDs for tag " << writeTag;
  }
}

bool AsyncFdSocket::FdSendMsgParamsCallback::registerFdsForWriteTag(
    WriteRequestTag writeTag, SocketFds::ToSend&& fds) {
  VLOG(5) << "this=" << this << ", registerFdsForWriteTag() on " << fds.size()
          << " FDs for tag " << writeTag;
  if (writeTag.empty()) {
    return false; // no insertion, error: we require a nonempty tag
  }
  auto [it, inserted] = writeTagToFds_.try_emplace(writeTag, std::move(fds));
  return inserted;
}

void AsyncFdSocket::FdSendMsgParamsCallback::destroyFdsForWriteTag(
    WriteRequestTag writeTag) noexcept {
  auto nh = writeTagToFds_.extract(writeTag);
  if (nh.empty()) {
    return; // Not every write has FDs; also, `wroteBytes` clears them.
  }
  VLOG(5) << "this=" << this << ", destroyFdsForWriteTag() on "
          << nh.mapped().size() << " FDs for tag " << writeTag;
}

namespace {

// Logs and returns `false` on error.
bool receiveFdsFromCMSG(
    const struct ::cmsghdr& cmsg, std::vector<folly::File>* fds) noexcept {
  if (cmsg.cmsg_len < CMSG_LEN(sizeof(int))) {
    LOG(ERROR) << "Got truncated SCM_RIGHTS message: length=" << cmsg.cmsg_len;
    return false;
  }
  const size_t dataLength = cmsg.cmsg_len - CMSG_LEN(0);

  const size_t numFDs = dataLength / sizeof(int);
  if ((dataLength % sizeof(int)) != 0) {
    LOG(ERROR) << "Non-integer number of file descriptors: size=" << dataLength;
    return false;
  }

  const auto* data = reinterpret_cast<const int*>(CMSG_DATA(&cmsg));
  for (size_t n = 0; n < numFDs; ++n) {
    auto fd = data[n];

// On Linux, `AsyncSocket` sets `MSG_CMSG_CLOEXEC` for us.
#if !defined(__linux__)
    int flags = ::fcntl(fd, F_GETFD);
    // On error, "fail open" by leaving the FD unmodified.
    if (flags == -1) [[unlikely]] {
      PLOG(ERROR) << "FdReadAncillaryDataCallback F_GETFD";
    } else if (-1 == ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC)) [[unlikely]] {
      PLOG(ERROR) << "FdReadAncillaryDataCallback F_SETFD";
    }
#endif // !Linux

    fds->emplace_back(fd, /*owns_fd*/ true);
  }

  return true;
}

// Logs and returns `false` on error.
bool receiveFds(struct ::msghdr& msg, std::vector<folly::File>* fds) noexcept {
  struct ::cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
  while (cmsg) {
    if (cmsg->cmsg_level != SOL_SOCKET) {
      LOG(ERROR) << "Unexpected cmsg_level=" << cmsg->cmsg_level;
      return false;
    } else if (cmsg->cmsg_type != SCM_RIGHTS) {
      LOG(ERROR) << "Unexpected cmsg_type=" << cmsg->cmsg_type;
      return false;
    } else {
      if (!receiveFdsFromCMSG(*cmsg, fds)) {
        return false;
      }
    }
    cmsg = CMSG_NXTHDR(&msg, cmsg);
  }
  return true;
}

} // namespace

void AsyncFdSocket::enqueueFdsFromAncillaryData(struct ::msghdr& msg) noexcept {
  eventBase_->dcheckIsInEventBaseThread();

  if (msg.msg_flags & MSG_CTRUNC) {
    AsyncSocketException ex(
        AsyncSocketException::INTERNAL_ERROR,
        "Got MSG_CTRUNC because the `AsyncFdSocket` buffer was too small");
    AsyncSocket::failRead(__func__, ex);
    return;
  }

  std::vector<folly::File> receivedFds;
  if (!receiveFds(msg, &receivedFds)) {
    AsyncSocketException ex(
        AsyncSocketException::INTERNAL_ERROR,
        "Failed to read FDs from msghdr.msg_control");
    AsyncSocket::failRead(__func__, ex);
    return;
  }

  // Don't waste queue space with empty FD lists since we match FDs only to
  // requests that claim to include FDs.
  if (receivedFds.empty()) {
    return;
  }

  const auto seqNum = receivedFdsSeqNum_;
  receivedFdsSeqNum_ =
      detail::addSocketFdsSeqNum(receivedFdsSeqNum_, receivedFds.size());
  SocketFds fds{std::move(receivedFds)};
  fds.setFdSocketSeqNumOnce(seqNum);

  VLOG(4) << "this=" << this << ", enqueueFdsFromAncillaryData() got "
          << fds.size() << " FDs with seq num " << seqNum
          << ", prev queue size " << fdsQueue_.size();

  fdsQueue_.emplace(std::move(fds));
}

#endif // !Windows

SocketFds AsyncFdSocket::popNextReceivedFds() {
#if defined(_WIN32)
  return SocketFds{};
#else
  eventBase_->dcheckIsInEventBaseThread();

  DCHECK_EQ(&readAncillaryDataCob_, getReadAncillaryDataCallback());
  if (fdsQueue_.empty()) {
    return SocketFds{};
  }
  auto fds = std::move(fdsQueue_.front());
  fdsQueue_.pop();
  return fds;
#endif // !Windows
}

SocketFds::SeqNum AsyncFdSocket::injectSocketSeqNumIntoFdsToSend(
    SocketFds* fds) {
#if defined(_WIN32)
  (void)fds;
  return SocketFds::kNoSeqNum;
#else
  if (fds->empty()) [[unlikely]] {
    LOG(DFATAL) << "Cannot inject sequence number into empty SocketFDs";
    return SocketFds::kNoSeqNum;
  }
  eventBase_->dcheckIsInEventBaseThread();
  const auto fdsSeqNum = allocatedToSendFdsSeqNum_;
  fds->dcheckToSendOrEmpty().setFdSocketSeqNumOnce(fdsSeqNum);
  allocatedToSendFdsSeqNum_ =
      detail::addSocketFdsSeqNum(allocatedToSendFdsSeqNum_, fds->size());
  return fdsSeqNum;
#endif // !Windows
}

} // namespace folly
