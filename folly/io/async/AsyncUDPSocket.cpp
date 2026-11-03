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

#include <folly/io/async/AsyncUDPSocket.h>

#include <algorithm>
#include <cerrno>

#include <boost/preprocessor/control/if.hpp>

#include <folly/Likely.h>
#include <folly/Utility.h>
#include <folly/io/SocketOptionMap.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/Fcntl.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/Unistd.h>
#include <folly/small_vector.h>

// Due to the way kernel headers are included, this may or may not be defined.
// Number pulled from 3.10 kernel headers.
#ifndef SO_REUSEPORT
#define SO_REUSEPORT 15
#endif

#if FOLLY_HAVE_VLA
#define FOLLY_HAVE_VLA_01 1
#else
#define FOLLY_HAVE_VLA_01 0
#endif

// xplat UDP GSO socket options.
#ifdef _WIN32
#ifndef UDP_SEND_MSG_SIZE
#define UDP_SEND_MSG_SIZE 2
#endif
#define UDP_GSO_SOCK_OPT_LEVEL IPPROTO_UDP
#define UDP_GSO_SOCK_OPT_TYPE UDP_SEND_MSG_SIZE
#define GSO_OPT_TYPE DWORD
#else /* !_WIN32 */
#define UDP_GSO_SOCK_OPT_LEVEL SOL_UDP
#define UDP_GSO_SOCK_OPT_TYPE UDP_SEGMENT
#define GSO_OPT_TYPE uint16_t
#endif /* _WIN32 */

namespace fsp = folly::portability::sockets;

namespace folly {

void AsyncUDPSocket::fromMsg(
    [[maybe_unused]] ReadCallback::OnDataAvailableParams& params,
    [[maybe_unused]] struct msghdr& msg) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  struct cmsghdr* cmsg;
  uint16_t* grosizeptr;
  for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_UDP) {
      if (cmsg->cmsg_type == UDP_GRO) {
        grosizeptr = (uint16_t*)CMSG_DATA(cmsg);
        params.gro = *grosizeptr;
      }
    } else if (cmsg->cmsg_level == SOL_SOCKET) {
      if (cmsg->cmsg_type == SO_TIMESTAMPING ||
          cmsg->cmsg_type == SO_TIMESTAMPNS) {
        // SO_TIMESTAMPING contains three timespecs; SO_TIMESTAMPNS only one.
        ReadCallback::OnDataAvailableParams::Timestamp ts{};
        memcpy(
            &ts,
            CMSG_DATA(cmsg),
            std::min<size_t>(cmsg->cmsg_len - CMSG_LEN(0), sizeof(ts)));
        params.ts = ts;
      }
    } else if (
        (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_TOS) ||
        (cmsg->cmsg_level == SOL_IPV6 && cmsg->cmsg_type == IPV6_TCLASS)) {
      params.tos = *(uint8_t*)CMSG_DATA(cmsg);
    } else if (
        (cmsg->cmsg_level == SOL_IP && cmsg->cmsg_type == IP_TTL) ||
        (cmsg->cmsg_level == SOL_IPV6 && cmsg->cmsg_type == IPV6_HOPLIMIT)) {
      int ttl;
      memcpy(&ttl, CMSG_DATA(cmsg), sizeof(ttl));
      params.ttl = static_cast<uint8_t>(ttl);
    } else if (
        cmsg->cmsg_level == IPPROTO_IPV6 && cmsg->cmsg_type == IPV6_PKTINFO) {
      auto* pktinfo = reinterpret_cast<struct in6_pktinfo*>(CMSG_DATA(cmsg));
      folly::SocketAddress addr;
      sockaddr_in6 sa{};
      sa.sin6_family = AF_INET6;
      sa.sin6_addr = pktinfo->ipi6_addr;
      addr.setFromSockaddr(reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
      params.localAddress = std::move(addr);
    } else if (
        cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
      auto* pktinfo = reinterpret_cast<struct in_pktinfo*>(CMSG_DATA(cmsg));
      folly::SocketAddress addr;
      sockaddr_in sa{};
      sa.sin_family = AF_INET;
      sa.sin_addr = pktinfo->ipi_addr;
      addr.setFromSockaddr(reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
      params.localAddress = std::move(addr);
    }
  }
#endif
}
static constexpr bool msgErrQueueSupported =
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    true;
#else
    false;
#endif // FOLLY_HAVE_MSG_ERRQUEUE

AsyncUDPSocket::AsyncUDPSocket(EventBase* evb)
    : EventHandler(evb),
      readCallback_(nullptr),
      eventBase_(evb),
      fd_(),
      zeroCopyBookkeeping_(std::make_shared<ZeroCopyFdBookkeeping>()),
      ioBufFreeFuncReleaseCb_(this) {
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }
}

AsyncUDPSocket::~AsyncUDPSocket() {
  if (fd_ != NetworkSocket()) {
    close();
  }
}

void AsyncUDPSocket::init(sa_family_t family, BindOptions bindOptions) {
  if (fd_ != NetworkSocket()) {
    // Already initialized.
    return;
  }

  NetworkSocket socket =
      netops::socket(family, SOCK_DGRAM, family != AF_UNIX ? IPPROTO_UDP : 0);
  if (socket == NetworkSocket()) {
    throw AsyncSocketException(
        AsyncSocketException::NOT_OPEN,
        "error creating async udp socket",
        errno);
  }

  auto g = folly::makeGuard([&] { netops::close(socket); });

  // put the socket in non-blocking mode
  int ret = netops::set_socket_non_blocking(socket);
  if (ret != 0) {
    throw AsyncSocketException(
        AsyncSocketException::NOT_OPEN,
        "failed to put socket in non-blocking mode",
        errno);
  }

  if (reuseAddr_) {
    // put the socket in reuse mode
    int value = 1;
    if (netops::setsockopt(
            socket, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) != 0) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "failed to put socket in reuse mode",
          errno);
    }
  }

  if (reusePort_) {
    // put the socket in port reuse mode
    int value = 1;
    auto opt = SO_REUSEPORT;
#ifdef _WIN32
    opt = SO_BROADCAST;
#endif
    if (netops::setsockopt(socket, SOL_SOCKET, opt, &value, sizeof(value)) !=
        0) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "failed to put socket in reuse_port mode",
          errno);
    }
  }

  if (freeBind_) {
    int optname = 0;
#if defined(IP_FREEBIND)
    optname = IP_FREEBIND;
#endif
    if (!optname) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN, "IP_FREEBIND is not supported");
    }
    // put the socket in free bind mode
    int value = 1;
    if (netops::setsockopt(
            socket, IPPROTO_IP, optname, &value, sizeof(value)) != 0) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "failed to put socket in free bind mode",
          errno);
    }
  }

  if (transparent_) {
    int optname = 0;
#if defined(IP_TRANSPARENT)
    optname = IP_TRANSPARENT;
#endif
    if (!optname) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN, "IP_TRANSPARENT is not supported");
    }
    // set the socket IP transparent mode
    int value = 1;
    if (netops::setsockopt(
            socket, IPPROTO_IP, optname, &value, sizeof(value)) != 0) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "failed to set socket IP transparent mode",
          errno);
    }
  }

  if (busyPollUs_ > 0) {
    int optname = 0;
#if defined(SO_BUSY_POLL)
    optname = SO_BUSY_POLL;
#endif
    if (!optname) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN, "SO_BUSY_POLL is not supported");
    }
    // Set busy_poll time in microseconds on the socket.
    // It sets how long socket will be in busy_poll mode when no event occurs.
    int value = busyPollUs_;
    if (netops::setsockopt(
            socket, SOL_SOCKET, optname, &value, sizeof(value)) != 0) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "failed to set SO_BUSY_POLL on the socket",
          errno);
    }
  }

  if (rcvBuf_ > 0) {
    // Set the size of the buffer for the received messages in rx_queues.
    int value = rcvBuf_;
    if (netops::setsockopt(
            socket, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) != 0) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "failed to set SO_RCVBUF on the socket",
          errno);
    }
  }

  if (sndBuf_ > 0) {
    // Set the size of the buffer for the sent messages in tx_queues.
    int value = sndBuf_;
    if (netops::setsockopt(
            socket, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)) != 0) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "failed to set SO_SNDBUF on the socket",
          errno);
    }
  }

  if (recvTos_) {
    // Set socket option to receive IPv6 Traffic Class/IPv4 Type of Service.
    int flag = 1;
    if (family == AF_INET6) {
      if (netops::setsockopt(
              socket, IPPROTO_IPV6, IPV6_RECVTCLASS, &flag, sizeof(flag)) !=
          0) {
        throw AsyncSocketException(
            AsyncSocketException::NOT_OPEN,
            "failed to set IPV6_RECVTCLASS on the socket",
            errno);
      }
    } else if (family == AF_INET) {
      if (netops::setsockopt(
              socket, IPPROTO_IP, IP_RECVTOS, &flag, sizeof(flag)) != 0) {
        throw AsyncSocketException(
            AsyncSocketException::NOT_OPEN,
            "failed to set IP_RECVTOS on the socket",
            errno);
      }
    }
  }

#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  if (recvTtl_) {
    int flag = 1;
    if (family == AF_INET6) {
      if (netops::setsockopt(
              socket, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &flag, sizeof(flag)) !=
          0) {
        throw AsyncSocketException(
            AsyncSocketException::NOT_OPEN,
            "failed to set IPV6_RECVHOPLIMIT on the socket",
            errno);
      }
    } else if (family == AF_INET) {
      if (netops::setsockopt(
              socket, IPPROTO_IP, IP_RECVTTL, &flag, sizeof(flag)) != 0) {
        throw AsyncSocketException(
            AsyncSocketException::NOT_OPEN,
            "failed to set IP_RECVTTL on the socket",
            errno);
      }
    }
  }

  if (recvDstAddr_) {
    // Set socket option to receive the destination address per packet via
    // IPV6_PKTINFO/IP_PKTINFO ancillary cmsgs.
    int flag = 1;
    if (family == AF_INET6) {
      if (netops::setsockopt(
              socket, IPPROTO_IPV6, IPV6_RECVPKTINFO, &flag, sizeof(flag)) !=
          0) {
        throw AsyncSocketException(
            AsyncSocketException::NOT_OPEN,
            "failed to set IPV6_RECVPKTINFO on the socket",
            errno);
      }
    } else if (family == AF_INET) {
      if (netops::setsockopt(
              socket, IPPROTO_IP, IP_PKTINFO, &flag, sizeof(flag)) != 0) {
        throw AsyncSocketException(
            AsyncSocketException::NOT_OPEN,
            "failed to set IP_PKTINFO on the socket",
            errno);
      }
    }
  }
#endif

  if (family == AF_INET6) {
    int flag = static_cast<int>(bindOptions.bindV6Only);
    if (netops::setsockopt(
            socket, IPPROTO_IPV6, IPV6_V6ONLY, &flag, sizeof(flag))) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN, "Failed to set IPV6_V6ONLY", errno);
    }
  }

  // success
  g.dismiss();
  fd_ = socket;
  ownership_ = FDOwnership::OWNS;

  // attach to EventHandler
  EventHandler::changeHandlerFD(fd_);
}

void AsyncUDPSocket::bind(
    const folly::SocketAddress& address, BindOptions bindOptions) {
  if (fd_ == NetworkSocket()) {
    init(address.getFamily(), bindOptions);
  }

  {
    // bind the socket to the interface
    int optname = 0;
#if defined(SO_BINDTODEVICE)
    optname = SO_BINDTODEVICE;
#endif
    auto& ifName = bindOptions.ifName;
    if (optname && !ifName.empty() &&
        netops::setsockopt(
            fd_, SOL_SOCKET, optname, ifName.data(), ifName.length())) {
      auto errnoCopy = errno;
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "failed to bind to device: " + ifName,
          errnoCopy);
    }
  }

  // bind to the address
  sockaddr_storage addrStorage;
  address.getAddress(&addrStorage);
  auto& saddr = reinterpret_cast<sockaddr&>(addrStorage);
  if (netops::bind(fd_, &saddr, address.getActualSize()) != 0) {
    throw AsyncSocketException(
        AsyncSocketException::NOT_OPEN,
        "failed to bind the async udp socket for:" + address.describe(),
        errno);
  }

  if (address.getFamily() == AF_UNIX || address.getPort() != 0) {
    localAddress_ = address;
  } else {
    localAddress_.setFromLocalAddress(fd_);
  }
}

void AsyncUDPSocket::connect(const folly::SocketAddress& address) {
  // not bound yet
  if (fd_ == NetworkSocket()) {
    init(address.getFamily());
  }

  sockaddr_storage addrStorage;
  address.getAddress(&addrStorage);
  if (netops::connect(
          fd_,
          reinterpret_cast<sockaddr*>(&addrStorage),
          address.getActualSize()) != 0) {
    throw AsyncSocketException(
        AsyncSocketException::NOT_OPEN,
        "Failed to connect the udp socket to:" + address.describe(),
        errno);
  }
  connected_ = true;
  connectedAddress_ = address;

  localAddress_.setFromLocalAddress(fd_);
}

void AsyncUDPSocket::dontFragment(bool df) {
  int optname4 = 0;
  int optval4 = df ? 0 : 0;
  int optname6 = 0;
  int optval6 = df ? 0 : 0;
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DO) && \
    defined(IP_PMTUDISC_WANT)
  optname4 = IP_MTU_DISCOVER;
  optval4 = df ? IP_PMTUDISC_DO : IP_PMTUDISC_WANT;
#endif
#if defined(IPV6_MTU_DISCOVER) && defined(IPV6_PMTUDISC_DO) && \
    defined(IPV6_PMTUDISC_WANT)
  optname6 = IPV6_MTU_DISCOVER;
  optval6 = df ? IPV6_PMTUDISC_DO : IPV6_PMTUDISC_WANT;
#endif
  if (optname4 && optval4 && address().getFamily() == AF_INET) {
    if (netops::setsockopt(
            fd_, IPPROTO_IP, optname4, &optval4, sizeof(optval4))) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "Failed to set DF with IP_MTU_DISCOVER",
          errno);
    }
  }
  if (optname6 && optval6 && address().getFamily() == AF_INET6) {
    if (netops::setsockopt(
            fd_, IPPROTO_IPV6, optname6, &optval6, sizeof(optval6))) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "Failed to set DF with IPV6_MTU_DISCOVER",
          errno);
    }
  }
}

void AsyncUDPSocket::setDFAndTurnOffPMTU() {
  int optname4 = 0;
  int optval4 = 0;
  int optname6 = 0;
  int optval6 = 0;
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_PROBE)
  optname4 = IP_MTU_DISCOVER;
  optval4 = IP_PMTUDISC_PROBE;
#endif
#if defined(IPV6_MTU_DISCOVER) && defined(IPV6_PMTUDISC_PROBE)
  optname6 = IPV6_MTU_DISCOVER;
  optval6 = IPV6_PMTUDISC_PROBE;
#endif
#if defined(_WIN32) && defined(IP_DONTFRAGMENT) && defined(IPV6_DONTFRAG)
  optname4 = IP_DONTFRAGMENT;
  optval4 = TRUE;
  optname6 = IPV6_DONTFRAG;
  optval6 = TRUE;
#endif
  if (optname4 && optval4 && address().getFamily() == AF_INET) {
    if (folly::netops::setsockopt(
            fd_, IPPROTO_IP, optname4, &optval4, sizeof(optval4))) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "Failed to turn off fragmentation and PMTU discovery (IPv4)",
          errno);
    }
  }
  if (optname6 && optval6 && address().getFamily() == AF_INET6) {
    if (folly::netops::setsockopt(
            fd_, IPPROTO_IPV6, optname6, &optval6, sizeof(optval6))) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN,
          "Failed to turn off fragmentation and PMTU discovery (IPv6)",
          errno);
    }
  }
}

void AsyncUDPSocket::setErrMessageCallback(
    ErrMessageCallback* errMessageCallback) {
  int optname4 = 0;
  int optname6 = 0;
#if defined(IP_RECVERR)
  optname4 = IP_RECVERR;
#endif
#if defined(IPV6_RECVERR)
  optname6 = IPV6_RECVERR;
#endif
  errMessageCallback_ = errMessageCallback;
  int err = (errMessageCallback_ != nullptr);
  if (optname4 && address().getFamily() == AF_INET &&
      netops::setsockopt(fd_, IPPROTO_IP, optname4, &err, sizeof(err))) {
    throw AsyncSocketException(
        AsyncSocketException::NOT_OPEN, "Failed to set IP_RECVERR", errno);
  }
  if (optname6 && address().getFamily() == AF_INET6 &&
      netops::setsockopt(fd_, IPPROTO_IPV6, optname6, &err, sizeof(err))) {
    throw AsyncSocketException(
        AsyncSocketException::NOT_OPEN, "Failed to set IPV6_RECVERR", errno);
  }
}

void AsyncUDPSocket::setFD(NetworkSocket fd, FDOwnership ownership) {
  CHECK_EQ(NetworkSocket(), fd_) << "Already bound to another FD";

  fd_ = fd;
  ownership_ = ownership;

  EventHandler::changeHandlerFD(fd_);
  localAddress_.setFromLocalAddress(fd_);
}

std::unique_ptr<AsyncUDPSocket> AsyncUDPSocket::createPeerOnSameFd() {
  CHECK_NE(NetworkSocket(), fd_)
      << "createPeerOnSameFd called before this socket is bound";
  auto peer = std::make_unique<AsyncUDPSocket>(eventBase_);
  peer->zeroCopyBookkeeping_ = zeroCopyBookkeeping_;
  peer->setFD(fd_, FDOwnership::SHARED);
  peer->setZeroCopy(true);
  return peer;
}

bool AsyncUDPSocket::setZeroCopy(bool enable) {
  if (msgErrQueueSupported) {
    zeroCopyVal_ = enable;

    if (fd_ == NetworkSocket()) {
      return false;
    }

    int val = enable ? 1 : 0;
    int ret =
        netops::setsockopt(fd_, SOL_SOCKET, SO_ZEROCOPY, &val, sizeof(val));

    // if enable == false, set zeroCopyEnabled_ = false regardless
    // if SO_ZEROCOPY is set or not
    if (!enable) {
      zeroCopyEnabled_ = enable;
      return true;
    }

    /* if the setsockopt failed, try to see if the socket inherited the flag
     * since we cannot set SO_ZEROCOPY on a socket s = accept
     */
    if (ret) {
      val = 0;
      socklen_t optlen = sizeof(val);
      ret = netops::getsockopt(fd_, SOL_SOCKET, SO_ZEROCOPY, &val, &optlen);

      if (!ret) {
        enable = val != 0;
      }
    }

    if (!ret) {
      zeroCopyEnabled_ = enable;

      return true;
    }
  }

  return false;
}

ssize_t AsyncUDPSocket::writeGSO(
    const folly::SocketAddress& address,
    const std::unique_ptr<folly::IOBuf>& buf,
    WriteOptions options) {
  return writeGSOWithCmsgs(address, buf, options, SocketCmsgMap{});
}

ssize_t AsyncUDPSocket::writeGSOWithCmsgs(
    const folly::SocketAddress& address,
    const std::unique_ptr<folly::IOBuf>& buf,
    WriteOptions options,
    const SocketCmsgMap& additionalCmsgs) {
  // UDP's typical MTU size is 1500, so high number of buffers
  //   really do not make sense. Optimize for buffer chains with
  //   buffers less than 16, which is the highest I can think of
  //   for a real use case.
  iovec vec[16];
  size_t iovec_len = buf->fillIov(vec, sizeof(vec) / sizeof(vec[0])).numIovecs;
  if (FOLLY_UNLIKELY(iovec_len == 0)) {
    buf->coalesce();
    vec[0].iov_base = const_cast<uint8_t*>(buf->data());
    vec[0].iov_len = buf->length();
    iovec_len = 1;
  }

  if (additionalCmsgs.empty()) {
    return writev(address, vec, iovec_len, options);
  }
  return writevWithCmsgs(address, vec, iovec_len, options, additionalCmsgs);
}

int AsyncUDPSocket::getZeroCopyFlags() {
  if (!zeroCopyEnabled_) {
    // if the zeroCopyReenableCounter_ is > 0
    // we try to dec and if we reach 0
    // we set zeroCopyEnabled_ to true
    if (zeroCopyReenableCounter_) {
      if (0 == --zeroCopyReenableCounter_) {
        zeroCopyEnabled_ = true;
        return MSG_ZEROCOPY;
      }
    }

    return 0;
  }

  return MSG_ZEROCOPY;
}

void AsyncUDPSocket::ZeroCopyFdBookkeeping::registerBuf(
    std::unique_ptr<folly::IOBuf>&& buf, ReleaseIOBufCallback* cb) noexcept {
  bufs_.emplace(nextId_, Entry{std::move(buf), cb});
  ++nextId_;
}

void AsyncUDPSocket::ZeroCopyFdBookkeeping::onCompletion(
    uint32_t lo, uint32_t hi) noexcept {
  // Walk [lo, hi] inclusive; check terminator before ++id to avoid uint32_t
  // overflow when hi == UINT32_MAX.
  for (uint32_t id = lo;; ++id) {
    auto it = bufs_.find(id);
    if (it != bufs_.end()) {
      auto entry = std::move(it->second);
      bufs_.erase(it);
      if (entry.cb) {
        entry.cb->releaseIOBuf(std::move(entry.buf));
      }
      // If no callback, entry.buf is dropped here.
    }
    // else: another bookkeeping on the same fd registered this id, or it
    // predates this bookkeeping's installation.
    if (id == hi) {
      break;
    }
  }
}

ssize_t AsyncUDPSocket::writeChain(
    const folly::SocketAddress& address,
    std::unique_ptr<folly::IOBuf>&& buf,
    WriteOptions options) {
  return writeChain(nullptr, address, std::move(buf), options);
}

ssize_t AsyncUDPSocket::writeChain(
    WriteCallback* wcb,
    const folly::SocketAddress& address,
    std::unique_ptr<folly::IOBuf>&& buf,
    WriteOptions options) {
  if (!nontrivialCmsgs_.empty()) {
    errno = EOPNOTSUPP;
    return -1;
  }
  int msg_flags = options.zerocopy ? getZeroCopyFlags() : 0;
  iovec vec[16];
  size_t iovec_len = buf->fillIov(vec, sizeof(vec) / sizeof(vec[0])).numIovecs;
  if (FOLLY_UNLIKELY(iovec_len == 0)) {
    buf->coalesce();
    vec[0].iov_base = const_cast<uint8_t*>(buf->data());
    vec[0].iov_len = buf->length();
    iovec_len = 1;
  }
  CHECK_NE(NetworkSocket(), fd_) << "Socket not yet bound";
  sockaddr_storage addrStorage;
  address.getAddress(&addrStorage);

  struct msghdr msg;
  if (!connected_) {
    msg.msg_name = reinterpret_cast<void*>(&addrStorage);
    msg.msg_namelen = address.getActualSize();
  } else {
    if (connectedAddress_ != address) {
      errno = ENOTSUP;
      return -1;
    }
    msg.msg_name = nullptr;
    msg.msg_namelen = 0;
  }
  msg.msg_iov = const_cast<struct iovec*>(vec);
  msg.msg_iovlen = iovec_len;
  msg.msg_control = nullptr;
  msg.msg_controllen = 0;
  msg.msg_flags = 0;

#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  char control
      [CMSG_SPACE(sizeof(uint16_t)) + /*gso*/
       CMSG_SPACE(sizeof(uint64_t)) /*txtime*/
  ] = {};
  msg.msg_control = control;
  struct cmsghdr* cm = nullptr;
  if (options.gso > 0) {
    msg.msg_controllen = CMSG_SPACE(sizeof(uint16_t));
    cm = CMSG_FIRSTHDR(&msg);

    cm->cmsg_level = SOL_UDP;
    cm->cmsg_type = UDP_SEGMENT;
    cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
    auto gso_len = static_cast<uint16_t>(options.gso);
    memcpy(CMSG_DATA(cm), &gso_len, sizeof(gso_len));
  }

  if (options.txTime.count() > 0 && txTime_.has_value() &&
      (txTime_.value().clockid >= 0)) {
    msg.msg_controllen += CMSG_SPACE(sizeof(uint64_t));
    if (cm) {
      cm = CMSG_NXTHDR(&msg, cm);
    } else {
      cm = CMSG_FIRSTHDR(&msg);
    }
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_TXTIME;
    cm->cmsg_len = CMSG_LEN(sizeof(uint64_t));

    struct timespec ts;
    clock_gettime(txTime_.value().clockid, &ts);
    uint64_t txtime = ts.tv_sec * 1000000000ULL + ts.tv_nsec +
        std::chrono::nanoseconds(options.txTime).count();
    memcpy(CMSG_DATA(cm), &txtime, sizeof(txtime));
  }
#else
  if (options.gso >= 1 || options.txTime.count() >= 1) {
    errno = EOPNOTSUPP;
    return -1;
  }
#endif

  auto ret = sendmsg(fd_, &msg, msg_flags);
  if (msg_flags) {
    if (ret < 0) {
      if (errno == ENOBUFS) {
        LOG(INFO) << "ENOBUFS...";
        // workaround for running with zerocopy enabled but without a big enough
        // memlock value - see ulimit -l
        // Also see /proc/sys/net/core/optmem_max
        zeroCopyEnabled_ = false;
        zeroCopyReenableCounter_ = zeroCopyReenableThreshold_;

        ret = sendmsg(fd_, &msg, 0);
      }
    } else {
      // Successful zerocopy send: register the buf so it stays alive until
      // the kernel emits a completion. Pick the release callback in this
      // order: the per-write WriteCallback (if any), then ioBufFreeFunc_
      // (if set) via the per-socket adapter, then nullptr (buf is dropped
      // on completion).
      auto* releaseCb = wcb ? wcb->getReleaseIOBufCallback() : nullptr;
      if (!releaseCb && ioBufFreeFunc_) {
        releaseCb = &ioBufFreeFuncReleaseCb_;
      }
      zeroCopyBookkeeping_->registerBuf(std::move(buf), releaseCb);
    }
  }

  if (ioBufFreeFunc_ && buf) {
    ioBufFreeFunc_(std::move(buf));
  }

  return ret;
} // namespace folly

ssize_t AsyncUDPSocket::write(
    const folly::SocketAddress& address,
    const std::unique_ptr<folly::IOBuf>& buf) {
  return writeGSO(
      address, buf, WriteOptions(0 /*gsoVal*/, false /* zerocopyVal*/));
}

ssize_t AsyncUDPSocket::writeWithCmsgsImpl(
    netops::Msgheader* msg,
    WriteOptions options,
    [[maybe_unused]] const SocketCmsgMap& additionalCmsgs) {
#if defined(FOLLY_HAVE_MSG_ERRQUEUE) || defined(_WIN32)
  XPLAT_CMSGHDR* cm = nullptr;
  for (const auto& itr : additionalCmsgs) {
    const auto key = itr.first;
    const auto val = itr.second;
    msg->incrCmsgLen(sizeof(val));
    cm = msg->getFirstOrNextCmsgHeader(cm);
    if (cm) {
      cm->cmsg_level = key.level;
      cm->cmsg_type = key.optname;
      cm->cmsg_len = F_CMSG_LEN(sizeof(val));
      F_COPY_CMSG_INT_DATA(cm, &val, sizeof(val));
    }
  }
  for (const auto& itr : *cmsgs_) {
    if (additionalCmsgs.find(itr.first) != additionalCmsgs.end()) {
      continue;
    }
    const auto key = itr.first;
    const auto val = itr.second;
    msg->incrCmsgLen(sizeof(val));
    cm = msg->getFirstOrNextCmsgHeader(cm);
    if (cm) {
      cm->cmsg_level = key.level;
      cm->cmsg_type = key.optname;
      cm->cmsg_len = F_CMSG_LEN(sizeof(val));
      F_COPY_CMSG_INT_DATA(cm, &val, sizeof(val));
    }
  }
  for (const auto& itr : nontrivialCmsgs_) {
    const auto& key = itr.first;
    const auto& val = itr.second;
    msg->incrCmsgLen(val.size());
    cm = msg->getFirstOrNextCmsgHeader(cm);
    if (cm) {
      cm->cmsg_level = key.level;
      cm->cmsg_type = key.optname;
      cm->cmsg_len = F_CMSG_LEN(val.size());
      F_COPY_CMSG_INT_DATA(cm, val.data(), val.size());
    }
  }

  if (options.gso > 0) {
    msg->incrCmsgLen(sizeof(uint16_t));
    cm = msg->getFirstOrNextCmsgHeader(cm);
    if (cm) {
      cm->cmsg_level = UDP_GSO_SOCK_OPT_LEVEL;
      cm->cmsg_type = UDP_GSO_SOCK_OPT_TYPE;
      cm->cmsg_len = F_CMSG_LEN(sizeof(GSO_OPT_TYPE));
      auto gso_len = static_cast<GSO_OPT_TYPE>(options.gso);
      F_COPY_CMSG_INT_DATA(cm, &gso_len, sizeof(gso_len));
    }
  }
#ifdef SCM_TXTIME
  if (options.txTime.count() > 0 && txTime_.has_value() &&
      (txTime_.value().clockid >= 0)) {
    cm = msg->getFirstOrNextCmsgHeader(cm);
    if (cm) {
      cm->cmsg_level = SOL_SOCKET;
      cm->cmsg_type = SCM_TXTIME;
      cm->cmsg_len = F_CMSG_LEN(sizeof(uint64_t));
      struct timespec ts;
      clock_gettime(txTime_.value().clockid, &ts);
      uint64_t txtime = ts.tv_sec * 1000000000ULL + ts.tv_nsec +
          std::chrono::nanoseconds(options.txTime).count();
      F_COPY_CMSG_INT_DATA(cm, &txtime, sizeof(txtime));
    }
  }
#endif // SCM_TXTIME
#endif // FOLLY_HAVE_MSG_ERRQUEUE || _WIN32

#ifdef _WIN32
  return netops::wsaSendMsgDirect(fd_, msg->getMsg());
#else
  int msg_flags = options.zerocopy ? getZeroCopyFlags() : 0;
  auto ret = sendmsg(fd_, msg->getMsg(), msg_flags);
  if (msg_flags && ret < 0 && errno == ENOBUFS) {
    LOG(INFO) << "ENOBUFS...";
    // workaround for running with zerocopy enabled but without a big enough
    // memlock value - see ulimit -l
    // Also see /proc/sys/net/core/optmem_max
    zeroCopyEnabled_ = false;
    zeroCopyReenableCounter_ = zeroCopyReenableThreshold_;
    ret = sendmsg(fd_, msg->getMsg(), 0);
  }
  return ret;
#endif
}

ssize_t AsyncUDPSocket::writeWithCmsgs(
    const folly::SocketAddress& address,
    const std::unique_ptr<folly::IOBuf>& buf,
    const SocketCmsgMap& cmsgs) {
  CHECK_NE(NetworkSocket(), fd_) << "Socket not yet bound";
  if (!buf) {
    errno = EINVAL;
    return -1;
  }

  return writeGSOWithCmsgs(
      address, buf, WriteOptions(0 /*gsoVal*/, false /*zerocopyVal*/), cmsgs);
}

ssize_t AsyncUDPSocket::writev(
    const folly::SocketAddress& address,
    const struct iovec* vec,
    size_t iovec_len,
    WriteOptions options) {
  return writevWithCmsgs(address, vec, iovec_len, options, SocketCmsgMap{});
}

ssize_t AsyncUDPSocket::writevWithCmsgs(
    const folly::SocketAddress& address,
    const struct iovec* vec,
    size_t iovec_len,
    WriteOptions options,
    const SocketCmsgMap& additionalCmsgs) {
  CHECK_NE(NetworkSocket(), fd_) << "Socket not yet bound";
  netops::Msgheader msg{};
  sockaddr_storage addrStorage{};
  address.getAddress(&addrStorage);

  if (!connected_) {
    msg.setName(&addrStorage, address.getActualSize());
  } else {
    if (connectedAddress_ != address) {
      errno = ENOTSUP;
      return -1;
    }
    msg.setName(nullptr, 0);
  }
  msg.setIovecs(vec, iovec_len);
  msg.setCmsgPtr(nullptr);
  msg.setCmsgLen(0);
  msg.setFlags(0);

#if defined(FOLLY_HAVE_MSG_ERRQUEUE) || defined(_WIN32)
  maybeUpdateDynamicCmsgs();

  // Socket-level cmsgs are the baseline; the per-call map overrides them key
  // by key, the way an IP_TOS cmsg overrides inet->tos in the kernel. Only the
  // socket-level keys `additionalCmsgs` does not name add to the control
  // buffer.
  size_t numCmsgs = additionalCmsgs.size();
  for (const auto& itr : *cmsgs_) {
    if (additionalCmsgs.find(itr.first) == additionalCmsgs.end()) {
      ++numCmsgs;
    }
  }

  // An empty `additionalCmsgs` must still dispatch through writevImpl() so
  // that subclasses overriding it keep seeing ordinary writes.
  const auto sendMsg = [&]() {
    if (additionalCmsgs.empty()) {
      return writevImpl(&msg, options);
    }
    return writeWithCmsgsImpl(&msg, options, additionalCmsgs);
  };

  constexpr size_t kSmallSizeMax = 5;
  size_t controlBufSize = options.gso > 0 ? 1 : 0;
  controlBufSize +=
      numCmsgs * (CMSG_SPACE(sizeof(int)) / CMSG_SPACE(sizeof(uint16_t)));

  if (nontrivialCmsgs_.empty() && controlBufSize <= kSmallSizeMax) {
    // Avoid allocating 0 length array. Doing so leads to exceptions
    if (controlBufSize == 0) {
      return sendMsg();
    }

    // suppress "warning: variable length array 'control' is used [-Wvla]"
    FOLLY_PUSH_WARNING
    FOLLY_GNU_DISABLE_WARNING("-Wvla")
    // we will allocate this on the stack anyway even if we do not use it
    char control
        [(BOOST_PP_IF(FOLLY_HAVE_VLA_01, controlBufSize, kSmallSizeMax)) *
         (CMSG_SPACE(sizeof(uint16_t)))];
    memset(control, 0, sizeof(control));
    msg.setCmsgPtr(control);
    FOLLY_POP_WARNING
    return sendMsg();
  } else {
    controlBufSize *= CMSG_SPACE(sizeof(uint16_t));
    for (const auto& itr : nontrivialCmsgs_) {
      controlBufSize += CMSG_SPACE(itr.second.size());
    }
    std::unique_ptr<char[]> control(new char[controlBufSize]);
    memset(control.get(), 0, controlBufSize);
    msg.setCmsgPtr(control.get());
    return sendMsg();
  }
#else
  if (!additionalCmsgs.empty()) {
    errno = EOPNOTSUPP;
    return -1;
  }
  if (options.gso >= 1) {
    errno = EOPNOTSUPP;
    return -1;
  }
#ifdef _WIN32
  return netops::wsaSendMsgDirect(fd_, msg.getMsg());
#else
  return sendmsg(fd_, msg.getMsg(), 0);
#endif
#endif
}

ssize_t AsyncUDPSocket::writevImpl(
    netops::Msgheader* msg, WriteOptions options) {
  return writeWithCmsgsImpl(msg, options, SocketCmsgMap{});
}

ssize_t AsyncUDPSocket::writev(
    const folly::SocketAddress& address,
    const struct iovec* vec,
    size_t iovec_len) {
  return writev(
      address,
      vec,
      iovec_len,
      WriteOptions(0 /*gsoVal*/, false /* zerocopyVal*/));
}

/**
 * Send the data in buffers to destination. Returns the return code from
 * ::sendmmsg.
 */
int AsyncUDPSocket::writem(
    Range<SocketAddress const*> addrs,
    const std::unique_ptr<folly::IOBuf>* bufs,
    size_t count) {
  return writemGSO(addrs, bufs, count, nullptr);
}

int AsyncUDPSocket::writemGSO(
    Range<SocketAddress const*> addrs,
    const std::unique_ptr<folly::IOBuf>* bufs,
    size_t count,
    const WriteOptions* options) {
  int ret;
  constexpr size_t kSmallSizeMax = 40;
  char* controlPtr = nullptr;
#ifndef FOLLY_HAVE_MSG_ERRQUEUE
  if (options) {
    errno = EOPNOTSUPP;
    return -1;
  }
#endif
  maybeUpdateDynamicCmsgs();
  size_t singleControlBufSize = 1;
  singleControlBufSize +=
      cmsgs_->size() * (CMSG_SPACE(sizeof(int)) / CMSG_SPACE(sizeof(uint16_t)));
  size_t controlBufSize = count * singleControlBufSize;
  if (nontrivialCmsgs_.empty() && controlBufSize <= kSmallSizeMax) {
    // suppress "warning: variable length array 'vec' is used [-Wvla]"
    FOLLY_PUSH_WARNING
    FOLLY_GNU_DISABLE_WARNING("-Wvla")
    mmsghdr vec[BOOST_PP_IF(FOLLY_HAVE_VLA_01, count, kSmallSizeMax)];
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    // we will allocate this on the stack anyway even if we do not use it
    char control
        [(BOOST_PP_IF(FOLLY_HAVE_VLA_01, controlBufSize, kSmallSizeMax)) *
         (CMSG_SPACE(sizeof(uint16_t)))];
    memset(control, 0, sizeof(control));
    controlPtr = control;
#endif
    FOLLY_POP_WARNING
    ret = writeImplIOBufs(addrs, bufs, count, vec, options, controlPtr);
  } else {
    std::unique_ptr<mmsghdr[]> vec(new mmsghdr[count]);
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    controlBufSize *= (CMSG_SPACE(sizeof(uint16_t)));
    for (const auto& itr : nontrivialCmsgs_) {
      controlBufSize += CMSG_SPACE(itr.second.size());
    }
    std::unique_ptr<char[]> control(new char[controlBufSize]);
    memset(control.get(), 0, controlBufSize);
    controlPtr = control.get();
#endif
    ret = writeImplIOBufs(addrs, bufs, count, vec.get(), options, controlPtr);
  }

  return ret;
}

int AsyncUDPSocket::writemv(
    Range<SocketAddress const*> addrs,
    iovec* iov,
    size_t* numIovecsInBuffer,
    size_t count) {
  return writemGSOv(addrs, iov, numIovecsInBuffer, count, nullptr);
}

int AsyncUDPSocket::writemGSOv(
    Range<SocketAddress const*> addrs,
    iovec* iov,
    size_t* numIovecsInBuffer,
    size_t count,
    const WriteOptions* options) {
  int ret;
  constexpr size_t kSmallSizeMax = 40;
  char* controlPtr = nullptr;
#ifndef FOLLY_HAVE_MSG_ERRQUEUE
  if (options) {
    errno = EOPNOTSUPP;
    return -1;
  }
#endif
  maybeUpdateDynamicCmsgs();
  size_t singleControlBufSize = 1;
  singleControlBufSize +=
      cmsgs_->size() * (CMSG_SPACE(sizeof(int)) / CMSG_SPACE(sizeof(uint16_t)));
  size_t controlBufSize = count * singleControlBufSize;
  if (nontrivialCmsgs_.empty() && controlBufSize <= kSmallSizeMax) {
    // suppress "warning: variable length array 'vec' is used [-Wvla]"
    FOLLY_PUSH_WARNING
    FOLLY_GNU_DISABLE_WARNING("-Wvla")
    mmsghdr vec[BOOST_PP_IF(FOLLY_HAVE_VLA_01, count, kSmallSizeMax)];
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    // we will allocate this on the stack anyway even if we do not use it
    char control
        [(BOOST_PP_IF(FOLLY_HAVE_VLA_01, controlBufSize, kSmallSizeMax)) *
         (CMSG_SPACE(sizeof(uint16_t)))];
    memset(control, 0, sizeof(control));
    controlPtr = control;
#endif
    FOLLY_POP_WARNING
    ret = writeImpl(
        addrs, numIovecsInBuffer, iov, count, vec, options, controlPtr);
  } else {
    std::unique_ptr<mmsghdr[]> vec(new mmsghdr[count]);
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    controlBufSize *= (CMSG_SPACE(sizeof(uint16_t)));
    for (const auto& itr : nontrivialCmsgs_) {
      controlBufSize += CMSG_SPACE(itr.second.size());
    }
    std::unique_ptr<char[]> control(new char[controlBufSize]);
    memset(control.get(), 0, controlBufSize);
    controlPtr = control.get();
#endif
    ret = writeImpl(
        addrs, numIovecsInBuffer, iov, count, vec.get(), options, controlPtr);
  }

  return ret;
}

void AsyncUDPSocket::fillIoVec(
    const std::unique_ptr<folly::IOBuf>* bufs,
    struct iovec* iov,
    size_t* messageIovLens,
    size_t count,
    size_t iov_count) {
  size_t remaining = iov_count;
  size_t iov_pos = 0;
  for (size_t i = 0; i < count; i++) {
    messageIovLens[i] = bufs[i]->countChainElements();
    auto ret = bufs[i]->fillIov(&iov[iov_pos], remaining);
    size_t iovec_len = ret.numIovecs;
    remaining -= iovec_len;
    iov_pos += iovec_len;
  }
}

void AsyncUDPSocket::fillMsgVec(
    Range<full_sockaddr_storage*> addrs,
    size_t* messageIovLens,
    size_t count,
    struct mmsghdr* msgvec,
    struct iovec* iov,
    const WriteOptions* options,
    char* control) {
  auto addr_count = addrs.size();
  DCHECK(addr_count);

  size_t iov_pos = 0;
  for (size_t i = 0; i < count; i++) {
    // we can use remaining here to avoid calling countChainElements() again
    auto& msg = msgvec[i].msg_hdr;
    // if we have less addrs compared to count
    // we use the last addr
    if (i < addr_count) {
      msg.msg_name = reinterpret_cast<void*>(&addrs[i].storage);
      msg.msg_namelen = addrs[i].len;
    } else {
      msg.msg_name = reinterpret_cast<void*>(&addrs[addr_count - 1].storage);
      msg.msg_namelen = addrs[addr_count - 1].len;
    }
    msg.msg_iov = &iov[iov_pos];
    msg.msg_iovlen = messageIovLens[i];
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    size_t controlBufSize = 1 +
        cmsgs_->size() *
            (CMSG_SPACE(sizeof(int)) / CMSG_SPACE(sizeof(uint16_t)));
    // get the offset in the control buf allocated for this msg
    msg.msg_control =
        &control[i * controlBufSize * CMSG_SPACE(sizeof(uint16_t))];
    msg.msg_controllen = 0;
    struct cmsghdr* cm = nullptr;
    // handle socket options
    for (auto itr = cmsgs_->begin(); itr != cmsgs_->end(); ++itr) {
      const auto key = itr->first;
      const auto val = itr->second;
      msg.msg_controllen += CMSG_SPACE(sizeof(val));
      if (cm) {
        cm = CMSG_NXTHDR(&msg, cm);
      } else {
        cm = CMSG_FIRSTHDR(&msg);
      }
      if (cm) {
        cm->cmsg_level = key.level;
        cm->cmsg_type = key.optname;
        cm->cmsg_len = CMSG_LEN(sizeof(val));
        memcpy(CMSG_DATA(cm), &val, sizeof(val));
      }
    }
    for (const auto& itr : nontrivialCmsgs_) {
      const auto& key = itr.first;
      const auto& val = itr.second;
      msg.msg_controllen += CMSG_SPACE(val.size());
      if (cm) {
        cm = CMSG_NXTHDR(&msg, cm);
      } else {
        cm = CMSG_FIRSTHDR(&msg);
      }
      if (cm) {
        cm->cmsg_level = key.level;
        cm->cmsg_type = key.optname;
        cm->cmsg_len = CMSG_LEN(val.size());
        memcpy(CMSG_DATA(cm), val.data(), val.size());
      }
    }

    // handle GSO
    if (options && options[i].gso > 0) {
      msg.msg_controllen += CMSG_SPACE(sizeof(uint16_t));
      if (cm) {
        cm = CMSG_NXTHDR(&msg, cm);
      } else {
        cm = CMSG_FIRSTHDR(&msg);
      }
      if (cm) {
        cm->cmsg_level = SOL_UDP;
        cm->cmsg_type = UDP_SEGMENT;
        cm->cmsg_len = CMSG_LEN(sizeof(uint16_t));
        auto gso_len = static_cast<uint16_t>(options[i].gso);
        memcpy(CMSG_DATA(cm), &gso_len, sizeof(gso_len));
      }
    }
    // there may be control buffer allocated, but nothing to put into it
    // in this case, we null out the control fields
    if (!cm) {
      // no GSO, no socket options, null out control fields
      msg.msg_control = nullptr;
      msg.msg_controllen = 0;
    }
#else
    (void)options;
    (void)control;
    msg.msg_control = nullptr;
    msg.msg_controllen = 0;
#endif
    msg.msg_flags = 0;

    msgvec[i].msg_len = 0;

    iov_pos += messageIovLens[i];
  }
}

int AsyncUDPSocket::writeImplIOBufs(
    Range<SocketAddress const*> addrs,
    const std::unique_ptr<folly::IOBuf>* bufs,
    size_t count,
    struct mmsghdr* msgvec,
    const WriteOptions* options,
    char* control) {
  size_t iov_count = 0;
  for (size_t i = 0; i < count; i++) {
    iov_count += bufs[i]->countChainElements();
  }

  constexpr size_t kSmallSizeMax = 8;
  if (iov_count <= kSmallSizeMax) {
    // suppress "warning: variable length array 'vec' is used [-Wvla]"
    FOLLY_PUSH_WARNING
    FOLLY_GNU_DISABLE_WARNING("-Wvla")
    iovec iov[BOOST_PP_IF(FOLLY_HAVE_VLA_01, iov_count, kSmallSizeMax)];
    size_t messageIovLens[BOOST_PP_IF(FOLLY_HAVE_VLA_01, count, kSmallSizeMax)];
    FOLLY_POP_WARNING
    fillIoVec(bufs, iov, messageIovLens, count, iov_count);
    return writeImpl(
        addrs, messageIovLens, iov, count, msgvec, options, control);
  } else {
    std::unique_ptr<iovec[]> iov(new iovec[iov_count]);
    std::unique_ptr<size_t[]> messageIovLens(new size_t[count]);
    fillIoVec(bufs, iov.get(), messageIovLens.get(), count, iov_count);
    return writeImpl(
        addrs,
        messageIovLens.get(),
        iov.get(),
        count,
        msgvec,
        options,
        control);
  }
}

int AsyncUDPSocket::writeImpl(
    Range<SocketAddress const*> addrs,
    size_t* messageIovLens,
    struct iovec* iov,
    size_t count,
    struct mmsghdr* msgvec,
    const WriteOptions* options,
    char* control) {
  // most times we have a single destination addr
  auto addr_count = addrs.size();
  constexpr size_t kAddrCountMax = 1;
  small_vector<full_sockaddr_storage, kAddrCountMax> addrStorage(addr_count);

  for (size_t i = 0; i < addr_count; i++) {
    addrs[i].getAddress(&addrStorage[i].storage);
    addrStorage[i].len = folly::to_narrow(addrs[i].getActualSize());
  }

  fillMsgVec(
      range(addrStorage), messageIovLens, count, msgvec, iov, options, control);
  return sendmmsg(fd_, msgvec, count, 0);
}

ssize_t AsyncUDPSocket::recvmsg(struct msghdr* msg, int flags) {
  return netops::recvmsg(fd_, msg, flags);
}

int AsyncUDPSocket::recvmmsg(
    struct mmsghdr* msgvec,
    unsigned int vlen,
    unsigned int flags,
    struct timespec* timeout) {
  return netops::recvmmsg(fd_, msgvec, vlen, flags, timeout);
}

void AsyncUDPSocket::resumeRead(ReadCallback* cob) {
  CHECK(!readCallback_) << "Another read callback already installed";
  CHECK_NE(NetworkSocket(), fd_)
      << "UDP server socket not yet bind to an address";

  readCallback_ = CHECK_NOTNULL(cob);
  if (!updateRegistration()) {
    AsyncSocketException ex(
        AsyncSocketException::NOT_OPEN, "failed to register for accept events");

    readCallback_ = nullptr;
    cob->onReadError(ex);
    return;
  }
}

void AsyncUDPSocket::pauseRead() {
  // It is ok to pause an already paused socket
  readCallback_ = nullptr;
  updateRegistration();
}

void AsyncUDPSocket::close() {
  eventBase_->dcheckIsInEventBaseThread();

  // Unregister any events we are registered for
  unregisterHandler();

  if (fd_ != NetworkSocket() && ownership_ == FDOwnership::OWNS) {
    netops::close(fd_);
  }

  fd_ = NetworkSocket();

  if (readCallback_) {
    auto cob = readCallback_;
    readCallback_ = nullptr;
    cob->onReadClosed();
  }
}

void AsyncUDPSocket::handlerReady(uint16_t events) noexcept {
  if (events & (EventHandler::READ | EventHandler::WRITE)) {
    if (handleErrMessages()) {
      return;
    }
  }

  if (events & EventHandler::READ) {
    DCHECK(readCallback_);
    handleRead();
  }
}

bool AsyncUDPSocket::isZeroCopyMsg([[maybe_unused]] const cmsghdr& cmsg) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  if ((cmsg.cmsg_level == SOL_IP && cmsg.cmsg_type == IP_RECVERR) ||
      (cmsg.cmsg_level == SOL_IPV6 && cmsg.cmsg_type == IPV6_RECVERR)) {
    auto serr =
        reinterpret_cast<const struct sock_extended_err*>(CMSG_DATA(&cmsg));
    return (
        (serr->ee_errno == 0) && (serr->ee_origin == SO_EE_ORIGIN_ZEROCOPY));
  }
#endif
  return false;
}

void AsyncUDPSocket::processZeroCopyMsg([[maybe_unused]] const cmsghdr& cmsg) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  auto serr =
      reinterpret_cast<const struct sock_extended_err*>(CMSG_DATA(&cmsg));
  uint32_t hi = serr->ee_data;
  uint32_t lo = serr->ee_info;
  zeroCopyBookkeeping_->recordCompletion(
      lo, hi, (serr->ee_code & SO_EE_CODE_ZEROCOPY_COPIED) != 0);
  // disable zero copy if the buffer was actually copied
  if ((serr->ee_code & SO_EE_CODE_ZEROCOPY_COPIED) && zeroCopyEnabled_) {
    VLOG(2) << "AsyncSocket::processZeroCopyMsg(): setting "
            << "zeroCopyEnabled_ = false due to SO_EE_CODE_ZEROCOPY_COPIED "
            << "on " << fd_;
    zeroCopyEnabled_ = false;
  }

  zeroCopyBookkeeping_->onCompletion(lo, hi);
#endif
}

size_t AsyncUDPSocket::handleErrMessages() noexcept {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  if (errMessageCallback_ == nullptr && !zeroCopyBookkeeping_->hasPending()) {
    return 0;
  }
  uint8_t ctrl[1024];
  unsigned char data;
  struct msghdr msg;
  iovec entry;

  entry.iov_base = &data;
  entry.iov_len = sizeof(data);
  msg.msg_iov = &entry;
  msg.msg_iovlen = 1;
  msg.msg_name = nullptr;
  msg.msg_namelen = 0;
  msg.msg_control = ctrl;
  msg.msg_controllen = sizeof(ctrl);
  msg.msg_flags = 0;

  int ret;
  size_t num = 0;
  while (fd_ != NetworkSocket()) {
    ret = netops::recvmsg(fd_, &msg, MSG_ERRQUEUE);
    VLOG(5) << "AsyncSocket::handleErrMessages(): recvmsg returned " << ret;

    if (ret < 0) {
      if (errno != EAGAIN) {
        auto errnoCopy = errno;
        LOG(ERROR) << "::recvmsg exited with code " << ret
                   << ", errno: " << errnoCopy;
        AsyncSocketException ex(
            AsyncSocketException::INTERNAL_ERROR,
            "recvmsg() failed",
            errnoCopy);
        failErrMessageRead(ex);
      }
      return num;
    }

    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
         cmsg != nullptr && cmsg->cmsg_len != 0;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      ++num;
      if (isZeroCopyMsg(*cmsg)) {
        processZeroCopyMsg(*cmsg);
      } else if (errMessageCallback_) {
        errMessageCallback_->errMessage(*cmsg);
      }
      if (fd_ == NetworkSocket()) {
        // once the socket is closed there is no use for more read errors.
        return num;
      }
    }
  }
  return num;
#else
  return 0;
#endif
}

void AsyncUDPSocket::failErrMessageRead(const AsyncSocketException& ex) {
  if (errMessageCallback_ != nullptr) {
    ErrMessageCallback* callback = errMessageCallback_;
    errMessageCallback_ = nullptr;
    callback->errMessageError(ex);
  }
}

void AsyncUDPSocket::handleRead() noexcept {
  void* buf{nullptr};
  size_t len{0};

  if (handleErrMessages()) {
    return;
  }

  if (fd_ == NetworkSocket()) {
    // The socket may have been closed by the error callbacks.
    return;
  }
  if (readCallback_->shouldOnlyNotify()) {
    return readCallback_->onNotifyDataAvailable(*this);
  }

  size_t numReads = maxReadsPerEvent_ ? maxReadsPerEvent_ : size_t(-1);
  EventBase* originalEventBase = eventBase_;
  while (numReads-- && readCallback_ && eventBase_ == originalEventBase) {
    readCallback_->getReadBuffer(&buf, &len);
    if (buf == nullptr || len == 0) {
      AsyncSocketException ex(
          AsyncSocketException::BAD_ARGS,
          "AsyncUDPSocket::getReadBuffer() returned empty buffer");

      auto cob = readCallback_;
      readCallback_ = nullptr;
      updateRegistration();
      cob->onReadError(ex);
      return;
    }

    struct sockaddr_storage addrStorage;
    socklen_t addrLen = sizeof(addrStorage);
    memset(&addrStorage, 0, size_t(addrLen));
    auto rawAddr = reinterpret_cast<sockaddr*>(&addrStorage);
    rawAddr->sa_family = localAddress_.getFamily();

    ssize_t bytesRead;
    ReadCallback::OnDataAvailableParams params;

#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    bool use_gro = gro_.has_value() && (gro_.value() > 0);
    bool use_ts = ts_.has_value() && (ts_.value() > 0);
    if (use_gro || use_ts || recvTos_ || recvTtl_ || recvDstAddr_) {
      char control[ReadCallback::OnDataAvailableParams::kCmsgSpace] = {};

      struct msghdr msg = {};
      struct iovec iov = {};

      iov.iov_base = buf;
      iov.iov_len = len;

      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;

      msg.msg_name = rawAddr;
      msg.msg_namelen = addrLen;

      msg.msg_control = control;
      msg.msg_controllen = sizeof(control);

      bytesRead = netops::recvmsg(fd_, &msg, MSG_TRUNC);

      if (bytesRead >= 0) {
        addrLen = msg.msg_namelen;
        fromMsg(params, msg);
      }
    } else {
      bytesRead = netops::recvfrom(fd_, buf, len, MSG_TRUNC, rawAddr, &addrLen);
    }
#elif defined(_WIN32)
    WSABUF wBuf;
    wBuf.buf = (CHAR*)buf;
    wBuf.len = (ULONG)len;

    WSAMSG wMsg{};
    wMsg.dwBufferCount = 1;
    wMsg.lpBuffers = &wBuf;
    wMsg.name = rawAddr;
    wMsg.namelen = addrLen;
    wMsg.dwFlags = 0;

    if (recvTos_) {
      CHAR control[WSA_CMSG_SPACE(sizeof(INT))] = {0};
      WSABUF controlBuf;
      controlBuf.buf = control;
      controlBuf.len = sizeof(control);
      wMsg.Control = controlBuf;
    }

    bytesRead = netops::wsaRecvMesg(fd_, &wMsg);
    if (recvTos_ && bytesRead > 0) {
      PCMSGHDR cmsg = WSA_CMSG_FIRSTHDR(&wMsg);
      while (cmsg != NULL) {
        if ((cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_TOS) ||
            (cmsg->cmsg_level == IPPROTO_IPV6 &&
             cmsg->cmsg_type == IPV6_TCLASS)) {
          params.tos = *(PINT)WSA_CMSG_DATA(cmsg);
          break;
        }
        cmsg = WSA_CMSG_NXTHDR(&wMsg, cmsg);
      }
    }
#else
    bytesRead = netops::recvfrom(fd_, buf, len, MSG_TRUNC, rawAddr, &addrLen);
#endif
    if (bytesRead >= 0) {
      clientAddress_.setFromSockaddr(rawAddr, addrLen);

      if (bytesRead > 0) {
        bool truncated = false;
        if ((size_t)bytesRead > len) {
          truncated = true;
          bytesRead = ssize_t(len);
        }

        readCallback_->onDataAvailable(
            clientAddress_, size_t(bytesRead), truncated, params);
      }
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No data could be read without blocking the socket
        return;
      }

      AsyncSocketException ex(
          AsyncSocketException::INTERNAL_ERROR, "::recvfrom() failed", errno);

      // In case of UDP we can continue reading from the socket
      // even if the current request fails. We notify the user
      // so that he can do some logging/stats collection if he wants.
      auto cob = readCallback_;
      readCallback_ = nullptr;
      updateRegistration();
      cob->onReadError(ex);
      return;
    }
  }
}

bool AsyncUDPSocket::updateRegistration() noexcept {
  uint16_t flags = NONE;

  if (readCallback_) {
    flags |= READ;
  }

  return registerHandler(uint16_t(flags | PERSIST));
}

bool AsyncUDPSocket::setGSO(int val) {
#if defined(FOLLY_HAVE_MSG_ERRQUEUE) || defined(_WIN32)
  int ret = netops::setsockopt(
      fd_, UDP_GSO_SOCK_OPT_LEVEL, UDP_GSO_SOCK_OPT_TYPE, &val, sizeof(val));

  gso_ = ret ? -1 : val;

  return !ret;
#else
  (void)val;
  return false;
#endif
}

int AsyncUDPSocket::getGSO() {
  // check if we can return the cached value
  if (FOLLY_UNLIKELY(!gso_.has_value())) {
#if defined(FOLLY_HAVE_MSG_ERRQUEUE) || defined(_WIN32)
    int gso = -1;
    socklen_t optlen = sizeof(gso);
    if (!netops::getsockopt(
            fd_,
            UDP_GSO_SOCK_OPT_LEVEL,
            UDP_GSO_SOCK_OPT_TYPE,
            &gso,
            &optlen)) {
      gso_ = gso;
    } else {
      gso_ = -1;
    }
#else
    gso_ = -1;
#endif
  }

  return gso_.value();
}

bool AsyncUDPSocket::setGRO(bool bVal) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  int val = bVal ? 1 : 0;
  int ret = netops::setsockopt(fd_, SOL_UDP, UDP_GRO, &val, sizeof(val));

  gro_ = ret ? -1 : val;

  return !ret;
#else
  (void)bVal;
  return false;
#endif
}

// packet timestamping
int AsyncUDPSocket::getTimestamping() {
  // check if we can return the cached value
  if (FOLLY_UNLIKELY(!ts_.has_value())) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    int ts = -1;
    socklen_t optlen = sizeof(ts);
    if (!netops::getsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING, &ts, &optlen)) {
      ts_ = ts;
    } else {
      ts_ = -1;
    }
#else
    ts_ = -1;
#endif
  }

  return ts_.value();
}

bool AsyncUDPSocket::setTimestamping(int val) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  int ret =
      netops::setsockopt(fd_, SOL_SOCKET, SO_TIMESTAMPING, &val, sizeof(val));

  ts_ = ret ? -1 : val;

  return !ret;
#else
  (void)val;
  return false;
#endif
}

int AsyncUDPSocket::getGRO() {
  // check if we can return the cached value
  if (FOLLY_UNLIKELY(!gro_.has_value())) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    int gro = -1;
    socklen_t optlen = sizeof(gro);
    if (!netops::getsockopt(fd_, SOL_UDP, UDP_GRO, &gro, &optlen)) {
      gro_ = gro;
    } else {
      gro_ = -1;
    }
#else
    gro_ = -1;
#endif
  }

  return gro_.value();
}

AsyncUDPSocket::TXTime AsyncUDPSocket::getTXTime() {
  // check if we can return the cached value
  if (FOLLY_UNLIKELY(!txTime_.has_value())) {
    TXTime txTime;
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    folly::netops::sock_txtime val = {};
    socklen_t optlen = sizeof(val);
    if (!netops::getsockopt(fd_, SOL_SOCKET, SO_TXTIME, &val, &optlen)) {
      txTime.clockid = val.clockid;
      txTime.deadline = (val.flags & folly::netops::SOF_TXTIME_DEADLINE_MODE);
    }
#endif
    txTime_ = txTime;
  }

  return txTime_.value();
}

bool AsyncUDPSocket::setTXTime(TXTime txTime) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  folly::netops::sock_txtime val;
  val.clockid = txTime.clockid;
  val.flags = txTime.deadline ? folly::netops::SOF_TXTIME_DEADLINE_MODE : 0;
  int ret = netops::setsockopt(fd_, SOL_SOCKET, SO_TXTIME, &val, sizeof(val));

  txTime_ = ret ? TXTime() : txTime;

  return !ret;
#else
  (void)txTime;
  return false;
#endif
}

bool AsyncUDPSocket::setRxZeroChksum6([[maybe_unused]] bool bVal) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  if (address().getFamily() != AF_INET6) {
    return false;
  }

  int val = bVal ? 1 : 0;
  int ret =
      netops::setsockopt(fd_, SOL_UDP, UDP_NO_CHECK6_RX, &val, sizeof(val));
  return !ret;
#else
  return false;
#endif
}

bool AsyncUDPSocket::setTxZeroChksum6([[maybe_unused]] bool bVal) {
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
  if (address().getFamily() != AF_INET6) {
    return false;
  }

  int val = bVal ? 1 : 0;
  int ret =
      netops::setsockopt(fd_, SOL_UDP, UDP_NO_CHECK6_TX, &val, sizeof(val));
  return !ret;
#else
  return false;
#endif
}

void AsyncUDPSocket::setTosOrTrafficClass(uint8_t tosOrTclass) {
#ifdef _WIN32
  // For windows, we can only set the values 0 and 1 (for the ECN bits).
  // Any DSCP values have to be set via the QoS Policy
  auto ecn = tosOrTclass & 0x3;
  auto level = address().getFamily() == AF_INET6 ? IPPROTO_IPV6 : IPPROTO_IP;
  if (ecn == 0) {
    // Remove ECN cmsgs if any exist
    defaultCmsgs_.erase({level, IP_ECN});
  } else {
    defaultCmsgs_[{level, IP_ECN}] = ecn;
  }
#else
  int valInt = tosOrTclass;
  if (address().getFamily() == AF_INET6) {
    if (netops::setsockopt(
            fd_,
            IPPROTO_IPV6,
            IPV6_TCLASS,
            &valInt,
            socklen_t(sizeof(valInt))) != 0) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN, "Failed to set IPV6_TCLASS", errno);
    }
  } else {
    if (netops::setsockopt(
            fd_, IPPROTO_IP, IP_TOS, &valInt, socklen_t(sizeof(valInt))) != 0) {
      throw AsyncSocketException(
          AsyncSocketException::NOT_OPEN, "Failed to set IP_TOS", errno);
    }
  }
#endif
}

void AsyncUDPSocket::applyOptions(
    const SocketOptionMap& options, SocketOptionKey::ApplyPos pos) {
  auto result = applySocketOptions(fd_, options, pos);
  if (result != 0) {
    throw AsyncSocketException(
        AsyncSocketException::INTERNAL_ERROR,
        "failed to set socket option",
        result);
  }
}

void AsyncUDPSocket::detachEventBase() {
  DCHECK(eventBase_ && eventBase_->isInEventBaseThread());
  registerHandler(uint16_t(NONE));
  eventBase_ = nullptr;
  EventHandler::detachEventBase();
}

void AsyncUDPSocket::attachEventBase(folly::EventBase* evb) {
  DCHECK(!eventBase_);
  DCHECK(evb && evb->isInEventBaseThread());
  eventBase_ = evb;
  EventHandler::attachEventBase(evb);
  updateRegistration();
}

void AsyncUDPSocket::setCmsgs(const SocketCmsgMap& cmsgs) {
  defaultCmsgs_ = cmsgs;
}

void AsyncUDPSocket::setNontrivialCmsgs(
    const SocketNontrivialCmsgMap& nontrivialCmsgs) {
  nontrivialCmsgs_ = nontrivialCmsgs;
}

void AsyncUDPSocket::appendCmsgs(const SocketCmsgMap& cmsgs) {
  for (auto itr = cmsgs.begin(); itr != cmsgs.end(); ++itr) {
    defaultCmsgs_[itr->first] = itr->second;
  }
}

void AsyncUDPSocket::maybeUpdateDynamicCmsgs() noexcept {
  cmsgs_ = &defaultCmsgs_;
  if (additionalCmsgsFunc_) {
    auto additionalCmsgs = additionalCmsgsFunc_();
    if (additionalCmsgs && !additionalCmsgs.value().empty()) {
      dynamicCmsgs_ = std::move(additionalCmsgs.value());
      // Union with defaultCmsgs_ without overwriting values for overlapping
      // keys
      dynamicCmsgs_.insert(defaultCmsgs_.begin(), defaultCmsgs_.end());
      cmsgs_ = &dynamicCmsgs_;
    }
  }
}

void AsyncUDPSocket::appendNontrivialCmsgs(
    const SocketNontrivialCmsgMap& nontrivialCmsgs) {
  for (auto itr = nontrivialCmsgs.begin(); itr != nontrivialCmsgs.end();
       ++itr) {
    nontrivialCmsgs_[itr->first] = itr->second;
  }
}

} // namespace folly
