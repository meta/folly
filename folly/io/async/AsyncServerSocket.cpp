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

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <folly/io/async/AsyncServerSocket.h>

#include <sys/types.h>

#include <cerrno>
#include <cstring>

#include <folly/FileUtil.h>
#include <folly/GLog.h>
#include <folly/Portability.h>
#include <folly/SocketAddress.h>
#include <folly/String.h>
#include <folly/detail/SocketFastOpen.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/NotificationQueue.h>
#include <folly/portability/Fcntl.h>
#include <folly/portability/Sockets.h>
#include <folly/portability/Unistd.h>

namespace folly {

#ifndef TCP_SAVE_SYN
#define TCP_SAVE_SYN 27
#endif

#ifndef TCP_SAVED_SYN
#define TCP_SAVED_SYN 28
#endif

static constexpr bool msgErrQueueSupported =
#ifdef FOLLY_HAVE_MSG_ERRQUEUE
    true;
#else
    false;
#endif // FOLLY_HAVE_MSG_ERRQUEUE

static constexpr bool kSupportReflectTos = kIsLinuxActual;

AsyncServerSocket::AcceptCallback::~AcceptCallback() = default;

const uint32_t AsyncServerSocket::kDefaultMaxAcceptAtOnce;
const uint32_t AsyncServerSocket::kDefaultCallbackAcceptAtOnce;
const uint32_t AsyncServerSocket::kDefaultMaxMessagesInQueue;

void AsyncServerSocket::RemoteAcceptor::start(
    EventBase* eventBase, uint32_t maxAtOnce) {
  queue_.setMaxReadAtOnce(maxAtOnce);

  eventBase->runInEventBaseThread([eventBase, this]() {
    callback_->acceptStarted();
    queue_.startConsuming(eventBase);
  });
}

void AsyncServerSocket::RemoteAcceptor::stop(
    EventBase* eventBase, AcceptCallback* callback) {
  eventBase->runInEventBaseThread([callback, this]() {
    callback->acceptStopped();
    delete this;
  });
}

AtomicNotificationQueueTaskStatus AsyncServerSocket::NewConnMessage::operator()(
    RemoteAcceptor& acceptor) noexcept {
  if (isExpired()) {
    closeNoInt(fd);
    if (acceptor.connectionEventCallback_) {
      auto queueTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - timeBeforeEnqueue);
      acceptor.connectionEventCallback_->onConnectionDropped(
          fd,
          clientAddr,
          fmt::format(
              "Exceeded deadline for accepting connection socket ({} ms)",
              queueTimeout.count()));
    }
    return AtomicNotificationQueueTaskStatus::DISCARD;
  }
  if (acceptor.connectionEventCallback_) {
    acceptor.connectionEventCallback_->onConnectionDequeuedByAcceptorCallback(
        fd, clientAddr);
  }
  acceptor.callback_->connectionAccepted(fd, clientAddr, {timeBeforeEnqueue});
  return AtomicNotificationQueueTaskStatus::CONSUMED;
}

AtomicNotificationQueueTaskStatus AsyncServerSocket::ErrorMessage::operator()(
    RemoteAcceptor& acceptor) noexcept {
  auto ex = make_exception_wrapper<std::runtime_error>(msg);
  acceptor.callback_->acceptError(std::move(ex));
  return AtomicNotificationQueueTaskStatus::CONSUMED;
}

/*
 * AsyncServerSocket::BackoffTimeout
 */
class AsyncServerSocket::BackoffTimeout : public AsyncTimeout {
 public:
  // Disallow copy, move, and default constructors.
  BackoffTimeout(BackoffTimeout&&) = delete;
  explicit BackoffTimeout(AsyncServerSocket* socket)
      : AsyncTimeout(socket->getEventBase()), socket_(socket) {}

  void timeoutExpired() noexcept override { socket_->backoffTimeoutExpired(); }

 private:
  AsyncServerSocket* socket_;
};

/*
 * AsyncServerSocket methods
 */

AsyncServerSocket::AsyncServerSocket(EventBase* eventBase)
    : eventBase_(eventBase),
      accepting_(false),
      maxAcceptAtOnce_(kDefaultMaxAcceptAtOnce),
      maxNumMsgsInQueue_(kDefaultMaxMessagesInQueue),
      acceptRateAdjustSpeed_(0),
      acceptRate_(1),
      lastAccepTimestamp_(std::chrono::steady_clock::now()),
      numDroppedConnections_(0),
      callbackIndex_(0),
      backoffTimeout_(nullptr),
      callbacks_(),
      napiIdToCallback_(),
      keepAliveEnabled_(true),
      closeOnExec_(true) {
  disableTransparentTls();
}

void AsyncServerSocket::setShutdownSocketSet(
    const std::weak_ptr<ShutdownSocketSet>& wNewSS) {
  const auto newSS = wNewSS.lock();
  const auto shutdownSocketSet = wShutdownSocketSet_.lock();

  if (shutdownSocketSet == newSS) {
    return;
  }

  if (shutdownSocketSet) {
    for (auto& h : sockets_) {
      shutdownSocketSet->remove(h.socket_);
    }
  }

  if (newSS) {
    for (auto& h : sockets_) {
      newSS->add(h.socket_);
    }
  }

  wShutdownSocketSet_ = wNewSS;
}

AsyncServerSocket::~AsyncServerSocket() {
  assert(callbacks_.empty());
  assert(napiIdToCallback_.empty());
}

int AsyncServerSocket::stopAccepting(int shutdownFlags) {
  int result = 0;
  for (auto& handler : sockets_) {
    VLOG(10) << "AsyncServerSocket::stopAccepting " << this << handler.socket_;
  }
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  // When destroy is called, unregister and close the socket immediately.
  accepting_ = false;

  // Close the sockets in reverse order as they were opened to avoid
  // the condition where another process concurrently tries to open
  // the same port, succeed to bind the first socket but fails on the
  // second because it hasn't been closed yet.
  for (; !sockets_.empty(); sockets_.pop_back()) {
    auto& handler = sockets_.back();
    handler.unregisterHandler();
    if (const auto shutdownSocketSet = wShutdownSocketSet_.lock()) {
      shutdownSocketSet->close(handler.socket_);
    } else if (shutdownFlags >= 0) {
      result = shutdownNoInt(handler.socket_, shutdownFlags);
      pendingCloseSockets_.push_back(handler.socket_);
    } else {
      closeNoInt(handler.socket_);
    }
  }

  // Destroy the backoff timout.  This will cancel it if it is running.
  delete backoffTimeout_;
  backoffTimeout_ = nullptr;

  // Close all of the callback queues to notify them that they are being
  // destroyed.  No one should access the AsyncServerSocket any more once
  // destroy() is called.  However, clear out callbacks_ before invoking the
  // accept callbacks just in case.  This will potentially help us detect the
  // bug if one of the callbacks calls addAcceptCallback() or
  // removeAcceptCallback().
  std::vector<CallbackInfo> callbacksCopy;
  callbacks_.swap(callbacksCopy);
  napiIdToCallback_.clear();
  napiIdCallbackIndex_.clear();
  localCallbackIndex_ = -1;
  for (const auto& callback : callbacksCopy) {
    // consumer may not be set if we are running in primary event base
    if (callback.consumer) {
      DCHECK(callback.eventBase);
      callback.consumer->stop(callback.eventBase, callback.callback);
    } else {
      DCHECK(callback.callback);
      callback.callback->acceptStopped();
    }
  }

  return result;
}

void AsyncServerSocket::destroy() {
  stopAccepting();
  for (auto s : pendingCloseSockets_) {
    closeNoInt(s);
  }
  // Then call DelayedDestruction::destroy() to take care of
  // whether or not we need immediate or delayed destruction
  DelayedDestruction::destroy();
}

void AsyncServerSocket::attachEventBase(EventBase* eventBase) {
  assert(eventBase_ == nullptr);
  eventBase->dcheckIsInEventBaseThread();

  eventBase_ = eventBase;
  for (auto& handler : sockets_) {
    handler.attachEventBase(eventBase);
  }
}

void AsyncServerSocket::detachEventBase() {
  assert(eventBase_ != nullptr);
  eventBase_->dcheckIsInEventBaseThread();
  assert(!accepting_);

  eventBase_ = nullptr;
  for (auto& handler : sockets_) {
    handler.detachEventBase();
  }
}

void AsyncServerSocket::useExistingSockets(
    const std::vector<NetworkSocket>& fds) {
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  if (!sockets_.empty()) {
    throw std::invalid_argument(
        "cannot call useExistingSocket() on a "
        "AsyncServerSocket that already has a socket");
  }

  for (auto fd : fds) {
    // Set addressFamily_ from this socket.
    // Note that the socket may not have been bound yet, but
    // setFromLocalAddress() will still work and get the correct address family.
    // We will update addressFamily_ again anyway if bind() is called later.
    SocketAddress address;
    address.setFromLocalAddress(fd);

#if defined(__linux__)
    if (noTransparentTls_) {
      // Ignore return value, errors are ok
      __u8 optval = FOLLY_SO_TTLS_TRUSTED_VAL_ENCRYPTED;
      netops::setsockopt(
          fd, SOL_SOCKET, FOLLY_SO_TTLS_TRUSTED, &optval, sizeof(optval));
    }
#endif

    setupSocket(fd, address.getFamily());
    sockets_.emplace_back(eventBase_, fd, this, address.getFamily());
    sockets_.back().changeHandlerFD(fd);
  }
}

void AsyncServerSocket::useExistingSocket(NetworkSocket fd) {
  useExistingSockets({fd});
}

void AsyncServerSocket::bindSocket(
    NetworkSocket fd,
    const SocketAddress& address,
    bool isExistingSocket,
    const std::string& ifName) {
  sockaddr_storage addrStorage;
  address.getAddress(&addrStorage);
  auto saddr = reinterpret_cast<sockaddr*>(&addrStorage);

#if defined(__linux__)
  if (!ifName.empty() &&
      netops::setsockopt(
          fd, SOL_SOCKET, SO_BINDTODEVICE, ifName.c_str(), ifName.length())) {
    auto errnoCopy = errno;
    if (!isExistingSocket) {
      closeNoInt(fd);
    }
    folly::throwSystemErrorExplicit(
        errnoCopy, "failed to bind to device: " + ifName);
  }
#else
  (void)ifName;
#endif

  if (netops::bind(fd, saddr, address.getActualSize()) != 0) {
    if (errno != EINPROGRESS) {
      // Get a copy of errno so that it is not overwritten by subsequent calls.
      auto errnoCopy = errno;
      if (!isExistingSocket) {
        closeNoInt(fd);
      }
      folly::throwSystemErrorExplicit(
          errnoCopy,
          "failed to bind to async server socket: " + address.describe());
    }
  }

#if defined(__linux__)
  if (noTransparentTls_) {
    // Ignore return value, errors are ok
    __u8 optval = FOLLY_SO_TTLS_TRUSTED_VAL_ENCRYPTED;
    netops::setsockopt(
        fd, SOL_SOCKET, FOLLY_SO_TTLS_TRUSTED, &optval, sizeof(optval));
  }
#endif

  // If we just created this socket, update the EventHandler and set socket_
  if (!isExistingSocket) {
    sockets_.emplace_back(eventBase_, fd, this, address.getFamily());
  }
}

bool AsyncServerSocket::setZeroCopy(bool enable) {
  if (msgErrQueueSupported) {
    // save the enable flag here
    zeroCopyVal_ = enable;
    int val = enable ? 1 : 0;
    size_t num = 0;
    for (auto& s : sockets_) {
      int ret = netops::setsockopt(
          s.socket_, SOL_SOCKET, SO_ZEROCOPY, &val, sizeof(val));

      num += (0 == ret) ? 1 : 0;
    }

    return num != 0;
  }

  return false;
}

void AsyncServerSocket::bindInternal(
    const SocketAddress& address,
    const std::string& ifName,
    const SocketOptionMap& socketOptions) {
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  // useExistingSocket() may have been called to initialize socket_ already.
  // However, in the normal case we need to create a new socket now.
  // Don't set socket_ yet, so that socket_ will remain uninitialized if an
  // error occurs.
  NetworkSocket fd;
  if (sockets_.empty()) {
    fd = createSocket(address.getFamily(), socketOptions);
  } else if (sockets_.size() == 1) {
    if (address.getFamily() != sockets_[0].addressFamily_) {
      throw std::invalid_argument(
          "Attempted to bind address to socket with "
          "different address family");
    }
    fd = sockets_[0].socket_;
  } else {
    throw std::invalid_argument("Attempted to bind to multiple fds");
  }

  bindSocket(fd, address, !sockets_.empty(), ifName);
}

void AsyncServerSocket::bind(const SocketAddress& address) {
  bind(address, emptySocketOptionMap);
}

void AsyncServerSocket::bind(
    const SocketAddress& address, const SocketOptionMap& socketOptions) {
  bindInternal(address, "", socketOptions);
}

void AsyncServerSocket::bind(
    const SocketAddress& address, const std::string& ifName) {
  bind(address, ifName, emptySocketOptionMap);
}

void AsyncServerSocket::bind(
    const SocketAddress& address,
    const std::string& ifName,
    const SocketOptionMap& socketOptions) {
  bindInternal(address, ifName, socketOptions);
}

void AsyncServerSocket::bind(
    const std::vector<IPAddress>& ipAddresses, uint16_t port) {
  bind(ipAddresses, port, emptySocketOptionMap);
}

void AsyncServerSocket::bind(
    const std::vector<IPAddress>& ipAddresses,
    uint16_t port,
    const SocketOptionMap& socketOptions) {
  if (ipAddresses.empty()) {
    throw std::invalid_argument("No ip addresses were provided");
  }
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  for (const IPAddress& ipAddress : ipAddresses) {
    SocketAddress address(ipAddress.toFullyQualified(), port);
    auto fd = createSocket(address.getFamily(), socketOptions);

    bindSocket(fd, address, false, "");
  }
  if (sockets_.empty()) {
    throw std::runtime_error(
        "did not bind any async server socket for port and addresses");
  }
}

void AsyncServerSocket::bind(
    const std::vector<IPAddressIfNamePair>& addresses, uint16_t port) {
  bind(addresses, port, emptySocketOptionMap);
}

void AsyncServerSocket::bind(
    const std::vector<IPAddressIfNamePair>& addresses,
    uint16_t port,
    const SocketOptionMap& socketOptions) {
  if (addresses.empty()) {
    throw std::invalid_argument("No ip addresses were provided");
  }
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  for (const auto& addr : addresses) {
    SocketAddress address(addr.first.toFullyQualified(), port);
    auto fd = createSocket(address.getFamily(), socketOptions);

    bindSocket(fd, address, false, addr.second);
  }
  if (sockets_.empty()) {
    throw std::runtime_error(
        "did not bind any async server socket for port and addresses");
  }
}

void AsyncServerSocket::bind(uint16_t port) {
  bind(port, emptySocketOptionMap);
}

void AsyncServerSocket::bind(
    uint16_t port, const SocketOptionMap& socketOptions) {
  struct addrinfo hints{};
  struct addrinfo* res0;
  char sport[sizeof("65536")];

  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  snprintf(sport, sizeof(sport), "%u", port);

  // On Windows the value we need to pass to bind to all available
  // addresses is an empty string. Everywhere else, it's nullptr.
  constexpr const char* kWildcardNode = kIsWindows ? "" : nullptr;
  // patternlint-disable-next-line cpp-dns-deps
  if (getaddrinfo(kWildcardNode, sport, &hints, &res0)) {
    throw std::invalid_argument(
        "Attempted to bind address to socket with "
        "bad getaddrinfo");
  }

  SCOPE_EXIT {
    freeaddrinfo(res0);
  };

  auto setupAddress = [&](struct addrinfo* res) {
    auto s = netops::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    // IPv6/IPv4 may not be supported by the kernel
    if (s == NetworkSocket() && errno == EAFNOSUPPORT) {
      return;
    }
    CHECK_NE(s, NetworkSocket());

    try {
      setupSocket(s, res->ai_family, socketOptions);
    } catch (...) {
      closeNoInt(s);
      throw;
    }

    if (res->ai_family == AF_INET6) {
      int v6only = 1;
      CHECK(
          0 ==
          netops::setsockopt(
              s, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)));
    }

    // Bind to the socket
    if (netops::bind(s, res->ai_addr, socklen_t(res->ai_addrlen)) != 0) {
      folly::throwSystemError(
          errno,
          "failed to bind to async server socket for port ",
          SocketAddress::getPortFrom(res->ai_addr),
          " family ",
          SocketAddress::getFamilyNameFrom(res->ai_addr, "<unknown>"));
    }

#if defined(__linux__)
    if (noTransparentTls_) {
      // Ignore return value, errors are ok
      __u8 optval = FOLLY_SO_TTLS_TRUSTED_VAL_ENCRYPTED;
      netops::setsockopt(
          s, SOL_SOCKET, FOLLY_SO_TTLS_TRUSTED, &optval, sizeof(optval));
    }
#endif

    SocketAddress address;
    address.setFromLocalAddress(s);

    sockets_.emplace_back(eventBase_, s, this, address.getFamily());
  };

  const int kNumTries = 25;
  for (int tries = 1; true; tries++) {
    // Prefer AF_INET6 addresses. RFC 3484 mandates that getaddrinfo
    // should return IPv6 first and then IPv4 addresses, but glibc's
    // getaddrinfo(nullptr) with AI_PASSIVE returns:
    // - 0.0.0.0 (IPv4-only)
    // - :: (IPv6+IPv4) in this order
    // See: https://sourceware.org/bugzilla/show_bug.cgi?id=9981
    for (struct addrinfo* res = res0; res; res = res->ai_next) {
      if (res->ai_family == AF_INET6) {
        setupAddress(res);
      }
    }

    // If port == 0, then we should try to bind to the same port on ipv4 and
    // ipv6.  So if we did bind to ipv6, figure out that port and use it.
    if (sockets_.size() == 1 && port == 0) {
      SocketAddress address;
      address.setFromLocalAddress(sockets_.back().socket_);
      snprintf(sport, sizeof(sport), "%u", address.getPort());
      freeaddrinfo(res0);
      CHECK_EQ(0, getaddrinfo(nullptr, sport, &hints, &res0));
    }

    try {
      for (struct addrinfo* res = res0; res; res = res->ai_next) {
        if (res->ai_family != AF_INET6) {
          setupAddress(res);
        }
      }
    } catch (const std::system_error&) {
      // If we can't bind to the same port on ipv4 as ipv6 when using
      // port=0 then we will retry again before giving up after
      // kNumTries attempts.  We do this by closing the sockets that
      // were opened, then restarting from scratch.
      if (port == 0 && !sockets_.empty() && tries != kNumTries) {
        for (const auto& socket : sockets_) {
          if (socket.socket_ == NetworkSocket()) {
            continue;
          } else if (
              const auto shutdownSocketSet = wShutdownSocketSet_.lock()) {
            shutdownSocketSet->close(socket.socket_);
          } else {
            closeNoInt(socket.socket_);
          }
        }
        sockets_.clear();
        snprintf(sport, sizeof(sport), "%u", port);
        freeaddrinfo(res0);
        CHECK_EQ(0, getaddrinfo(nullptr, sport, &hints, &res0));
        continue;
      }

      throw;
    }

    break;
  }

  if (sockets_.empty()) {
    throw std::runtime_error("did not bind any async server socket for port");
  }
}

void AsyncServerSocket::setEnableReuseAddr(bool enable) {
  enableReuseAddr_ = enable;
  for (auto& handler : sockets_) {
    if (handler.socket_ == NetworkSocket()) {
      continue;
    }

    int val = (enable) ? 1 : 0;
    if (netops::setsockopt(
            handler.socket_, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) !=
        0) {
      auto errnoCopy = errno;
      LOG(ERROR) << "failed to set SO_REUSEADDR on async server socket "
                 << errnoCopy;
      folly::throwSystemErrorExplicit(
          errnoCopy, "failed to set SO_REUSEADDR on async server socket");
    }
  }
}

void AsyncServerSocket::setIPFreebind(bool enable) {
  // We defer setting this option to setupSocket to ensure it is done pre-bind.
  ipFreebind_ = enable;
}

void AsyncServerSocket::listen(int backlog) {
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  // Start listening
  for (auto& handler : sockets_) {
    if (netops::listen(handler.socket_, backlog) == -1) {
      folly::throwSystemError(errno, "failed to listen on async server socket");
    }
  }
}

void AsyncServerSocket::getAddress(SocketAddress* addressReturn) const {
  CHECK(!sockets_.empty());
  VLOG_IF(2, sockets_.size() > 1)
      << "Warning: getAddress() called and multiple addresses available ("
      << sockets_.size() << "). Returning only the first one.";

  addressReturn->setFromLocalAddress(sockets_[0].socket_);
}

std::vector<SocketAddress> AsyncServerSocket::getAddresses() const {
  CHECK(!sockets_.empty());
  auto tsaVec = std::vector<SocketAddress>(sockets_.size());
  auto tsaIter = tsaVec.begin();
  for (const auto& socket : sockets_) {
    (tsaIter++)->setFromLocalAddress(socket.socket_);
  }
  return tsaVec;
}

void AsyncServerSocket::addAcceptCallback(
    AcceptCallback* callback, EventBase* eventBase, uint32_t maxAtOnce) {
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  // If this is the first accept callback and we are supposed to be accepting,
  // start accepting once the callback is installed.
  bool runStartAccepting = accepting_ && callbacks_.empty();

  callbacks_.emplace_back(callback, eventBase);
  int napiId = -1;
  if (eventBase) {
    napiId = eventBase->getBackend()->getNapiId();
    if (napiId != -1) {
      napiIdToCallback_[napiId].emplace_back(callback, eventBase);
    }
  }

  SCOPE_SUCCESS {
    // If this is the first accept callback and we are supposed to be accepting,
    // start accepting.
    if (runStartAccepting) {
      startAccepting();
    }
  };

  if (!eventBase) {
    // Run in AsyncServerSocket's eventbase; notify that we are
    // starting to accept connections
    callback->acceptStarted();
    return;
  }

  // Start the remote acceptor.
  //
  // It would be nice if we could avoid starting the remote acceptor if
  // eventBase == eventBase_.  However, that would cause issues if
  // detachEventBase() and attachEventBase() were ever used to change the
  // primary EventBase for the server socket.  Therefore we require the caller
  // to specify a nullptr EventBase if they want to ensure that the callback is
  // always invoked in the primary EventBase, and to be able to invoke that
  // callback more efficiently without having to use a notification queue.
  RemoteAcceptor* acceptor = nullptr;
  try {
    acceptor = new RemoteAcceptor(callback, connectionEventCallback_);
    acceptor->start(eventBase, maxAtOnce);
  } catch (...) {
    callbacks_.pop_back();
    if (napiId != -1) {
      auto& vec = napiIdToCallback_[napiId];
      if (!vec.empty()) {
        vec.pop_back();
      }
      if (vec.empty()) {
        napiIdToCallback_.erase(napiId);
        napiIdCallbackIndex_.erase(napiId);
      }
    }
    delete acceptor;
    throw;
  }
  callbacks_.back().consumer = acceptor;
  if (napiId != -1) {
    if (auto it = napiIdToCallback_.find(napiId);
        it != napiIdToCallback_.end()) {
      it->second.back().consumer = acceptor;
    }
  }
  if (localCallbackIndex_ < 0 && callbacks_.back().eventBase == eventBase_) {
    localCallbackIndex_ = static_cast<int>(callbacks_.size() - 1);
  }
}

void AsyncServerSocket::removeAcceptCallback(
    AcceptCallback* callback, EventBase* eventBase) {
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  // Find the matching AcceptCallback.
  // We just do a simple linear search; we don't expect removeAcceptCallback()
  // to be called frequently, and we expect there to only be a small number of
  // callbacks anyway.
  auto it = callbacks_.begin();
  uint32_t n = 0;
  while (true) {
    if (it == callbacks_.end()) {
      throw std::runtime_error(
          "AsyncServerSocket::removeAcceptCallback(): "
          "accept callback not found");
    }
    if (it->callback == callback &&
        (it->eventBase == eventBase || eventBase == nullptr)) {
      break;
    }
    ++it;
    ++n;
  }

  // If the matching AcceptCallback is also tied to a specific NAPI ID, erase it
  // as well.
  for (auto mapIt = napiIdToCallback_.begin();
       mapIt != napiIdToCallback_.end();) {
    auto& vec = mapIt->second;
    for (auto vecIt = vec.begin(); vecIt != vec.end();) {
      if (vecIt->callback == callback &&
          (vecIt->eventBase == eventBase || eventBase == nullptr)) {
        vecIt = vec.erase(vecIt);
      } else {
        ++vecIt;
      }
    }
    if (vec.empty()) {
      napiIdCallbackIndex_.erase(mapIt->first);
      mapIt = napiIdToCallback_.erase(mapIt);
    } else {
      ++mapIt;
    }
  }

  // Remove this callback from callbacks_.
  //
  // Do this before invoking the acceptStopped() callback, in case
  // acceptStopped() invokes one of our methods that examines callbacks_.
  //
  // Save a copy of the CallbackInfo first.
  CallbackInfo info(*it);
  callbacks_.erase(it);
  if (n < callbackIndex_) {
    // We removed an element before callbackIndex_.  Move callbackIndex_ back
    // one step, since things after n have been shifted back by 1.
    --callbackIndex_;
  } else {
    // We removed something at or after callbackIndex_.
    // If we removed the last element and callbackIndex_ was pointing at it,
    // we need to reset callbackIndex_ to 0.
    if (callbackIndex_ >= callbacks_.size()) {
      callbackIndex_ = 0;
    }
  }

  if (info.consumer) {
    // consumer could be nullptr is we run callbacks in primary event
    // base
    DCHECK(info.eventBase);
    info.consumer->stop(info.eventBase, info.callback);
  } else {
    // callback invoked in the primary event base, just call directly
    DCHECK(info.callback);
    callback->acceptStopped();
  }

  // If we are supposed to be accepting but the last accept callback
  // was removed, unregister for events until a callback is added.
  if (accepting_ && callbacks_.empty()) {
    for (auto& handler : sockets_) {
      handler.unregisterHandler();
    }
  }
}

void AsyncServerSocket::startAccepting() {
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }

  accepting_ = true;
  if (callbacks_.empty()) {
    // We can't actually begin accepting if no callbacks are defined.
    // Wait until a callback is added to start accepting.
    return;
  }

  for (auto& handler : sockets_) {
    if (!handler.registerHandler(EventHandler::READ | EventHandler::PERSIST)) {
      throw std::runtime_error("failed to register for accept events");
    }
  }
}

void AsyncServerSocket::pauseAccepting() {
  if (eventBase_) {
    eventBase_->dcheckIsInEventBaseThread();
  }
  accepting_ = false;
  for (auto& handler : sockets_) {
    handler.unregisterHandler();
  }

  // If we were in the accept backoff state, disable the backoff timeout
  if (backoffTimeout_) {
    backoffTimeout_->cancelTimeout();
  }
}

NetworkSocket AsyncServerSocket::createSocket(
    int family, const SocketOptionMap& socketOptions) {
  auto fd = netops::socket(family, SOCK_STREAM, 0);
  if (fd == NetworkSocket()) {
    folly::throwSystemError(errno, "error creating async server socket");
  }

  try {
    setupSocket(fd, family, socketOptions);
  } catch (...) {
    closeNoInt(fd);
    throw;
  }
  return fd;
}

/**
 * Enable/Disable TOS reflection for the server socket
 * If enabled, the 'accepted' connections will reflect the
 * TOS derived from the client's connect request
 */
void AsyncServerSocket::setTosReflect(bool enable) {
  if (!kSupportReflectTos || !enable) {
    tosReflect_ = false;
    return;
  }

  for (auto& handler : sockets_) {
    if (handler.socket_ == NetworkSocket()) {
      continue;
    }

    int val = (enable) ? 1 : 0;
    int ret = netops::setsockopt(
        handler.socket_, IPPROTO_TCP, TCP_SAVE_SYN, &val, sizeof(val));

    if (ret == 0) {
      VLOG(10) << "Enabled SYN save for socket " << handler.socket_;
    } else {
      folly::throwSystemError(errno, "failed to enable TOS reflect");
    }
  }
  tosReflect_ = true;
}

void AsyncServerSocket::setListenerTos(uint32_t tos) {
  if (!kSupportReflectTos || tos == 0) {
    listenerTos_ = 0;
    return;
  }

  for (auto& handler : sockets_) {
    if (handler.socket_ == NetworkSocket()) {
      continue;
    }

    const auto proto =
        (handler.addressFamily_ == AF_INET) ? IPPROTO_IP : IPPROTO_IPV6;
    const auto optName =
        (handler.addressFamily_ == AF_INET) ? IP_TOS : IPV6_TCLASS;

    int ret =
        netops::setsockopt(handler.socket_, proto, optName, &tos, sizeof(tos));

    if (ret == 0) {
      VLOG(10) << "Set TOS " << tos << " for for socket " << handler.socket_;
    } else {
      folly::throwSystemError(errno, "failed to set TOS for socket");
    }
  }
  listenerTos_ = tos;
}

void AsyncServerSocket::setupSocket(
    NetworkSocket fd, int family, const SocketOptionMap& socketOptions) {
  // Apply caller-provided PRE_BIND socket options.
  for (const auto& opt : validateSocketOptions(
           socketOptions, family, SocketOptionKey::ApplyPos::PRE_BIND)) {
    if (opt.first.apply(fd, opt.second) != 0) {
      const auto errnoCopy = errno;
      LOG(ERROR)
          << "failed to apply socket option on async server socket"
          << " fd=" << fd << " level=" << opt.first.level
          << " optname=" << opt.first.optname << " applyPos=PRE_BIND"
          << " value=" << opt.second << ": " << errnoStr(errnoCopy);
    }
  }

  // Put the socket in non-blocking mode
  if (netops::set_socket_non_blocking(fd) != 0) {
    folly::throwSystemError(errno, "failed to put socket in non-blocking mode");
  }

  // Set reuseaddr to avoid 2MSL delay on server restart
  int one = 1;
  // AF_UNIX does not support SO_REUSEADDR, setting this would confuse Windows
  if (family != AF_UNIX && enableReuseAddr_ &&
      netops::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) !=
          0) {
    auto errnoCopy = errno;
    // This isn't a fatal error; just log an error message and continue
    LOG(ERROR) << "failed to set SO_REUSEADDR on async server socket "
               << errnoCopy;
  }

  // Set reuseport to support multiple accept threads
  int zero = 0;
  if (reusePortEnabled_ &&
      netops::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(int)) !=
          0) {
    auto errnoCopy = errno;
    LOG(ERROR) << "failed to set SO_REUSEPORT on async server socket "
               << errnoStr(errnoCopy);
#ifdef WIN32
    folly::throwSystemErrorExplicit(
        errnoCopy, "failed to set SO_REUSEPORT on async server socket");
#else
    SocketAddress address;
    address.setFromLocalAddress(fd);
    folly::throwSystemErrorExplicit(
        errnoCopy,
        "failed to set SO_REUSEPORT on async server socket: " +
            address.describe());
#endif
  }

  // Set keepalive as desired
  if (netops::setsockopt(
          fd,
          SOL_SOCKET,
          SO_KEEPALIVE,
          (keepAliveEnabled_) ? &one : &zero,
          sizeof(int)) != 0) {
    auto errnoCopy = errno;
    LOG(ERROR) << "failed to set SO_KEEPALIVE on async server socket: "
               << errnoStr(errnoCopy);
  }

  // Setup FD_CLOEXEC flag
  if (closeOnExec_ && (-1 == netops::set_socket_close_on_exec(fd))) {
    auto errnoCopy = errno;
    LOG(ERROR) << "failed to set FD_CLOEXEC on async server socket: "
               << errnoStr(errnoCopy);
  }

  // Set TCP nodelay if available, MAC OS X Hack
  // See http://lists.danga.com/pipermail/memcached/2005-March/001240.html
#ifndef TCP_NOPUSH
#if FOLLY_HAVE_VSOCK
  auto isVsock = family == AF_VSOCK;
#else
  auto isVsock = false;
#endif

  if (family != AF_UNIX && !isVsock) {
    if (netops::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) !=
        0) {
      auto errnoCopy = errno;
      // This isn't a fatal error; just log an error message and continue
      LOG(ERROR) << "failed to set TCP_NODELAY on async server socket: "
                 << errnoStr(errnoCopy);
    }
  }
#else
  (void)family; // to avoid unused parameter warning
#endif

#if FOLLY_ALLOW_TFO
  if (tfo_ && detail::tfo_enable(fd, tfoMaxQueueSize_) != 0) {
    auto errnoCopy = errno;
    // This isn't a fatal error; just log an error message and continue
    LOG(WARNING) << "failed to set TCP_FASTOPEN on async server socket: "
                 << folly::errnoStr(errnoCopy);
  }
#endif

  if (zeroCopyVal_) {
    int val = 1;
    int ret =
        netops::setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &val, sizeof(val));
    if (ret) {
      auto errnoCopy = errno;
      LOG(WARNING) << "failed to set SO_ZEROCOPY on async server socket: "
                   << folly::errnoStr(errnoCopy);
    }
  }

#if defined(__linux__)
  if (ipFreebind_ &&
      netops::setsockopt(fd, IPPROTO_IP, IP_FREEBIND, &one, sizeof(int)) != 0) {
    auto errnoCopy = errno;
    LOG(ERROR) << "failed to set IP_FREEBIND on async server socket: "
               << errnoStr(errnoCopy);
  }
#endif

  if (const auto shutdownSocketSet = wShutdownSocketSet_.lock()) {
    shutdownSocketSet->add(fd);
  }
}

void AsyncServerSocket::handlerReady(
    uint16_t /* events */,
    NetworkSocket fd,
    sa_family_t addressFamily) noexcept {
  assert(!callbacks_.empty());
  DestructorGuard dg(this);

  // Only accept up to maxAcceptAtOnce_ connections at a time,
  // to avoid starving other I/O handlers using this EventBase.
  for (uint32_t n = 0; n < maxAcceptAtOnce_; ++n) {
    SocketAddress address;

    sockaddr_storage addrStorage = {};
    socklen_t addrLen = sizeof(addrStorage);
    auto saddr = reinterpret_cast<sockaddr*>(&addrStorage);

    // In some cases, accept() doesn't seem to update these correctly.
    saddr->sa_family = addressFamily;
    if (addressFamily == AF_UNIX) {
      addrLen = sizeof(struct sockaddr_un);
    }

    // Accept a new client socket
#if FOLLY_HAVE_ACCEPT4
    auto clientSocket = NetworkSocket::fromFd(
        accept4(fd.toFd(), saddr, &addrLen, SOCK_NONBLOCK));
#else
    auto clientSocket = netops::accept(fd, saddr, &addrLen);
#endif

    address.setFromSockaddr(saddr, addrLen);

    if (clientSocket != NetworkSocket() && connectionEventCallback_) {
      connectionEventCallback_->onConnectionAccepted(clientSocket, address);
    }

    // Connection accepted, get the SYN packet from the client if
    // TOS reflect is enabled
    if (kSupportReflectTos && clientSocket != NetworkSocket() && tosReflect_) {
      std::array<uint32_t, 64> buffer;
      socklen_t len = sizeof(buffer);
      int ret = netops::getsockopt(
          clientSocket, IPPROTO_TCP, TCP_SAVED_SYN, &buffer, &len);

      if (ret == 0) {
        uint32_t tosWord = folly::Endian::big(buffer[0]);
        if (addressFamily == AF_INET6) {
          tosWord = (tosWord & 0x0FC00000) >> 20;
          // Set the TOS on the return socket only if it is non-zero
          if (tosWord) {
            ret = netops::setsockopt(
                clientSocket,
                IPPROTO_IPV6,
                IPV6_TCLASS,
                &tosWord,
                sizeof(tosWord));
          }
        } else if (addressFamily == AF_INET) {
          tosWord = (tosWord & 0x00FC0000) >> 16;
          if (tosWord) {
            ret = netops::setsockopt(
                clientSocket, IPPROTO_IP, IP_TOS, &tosWord, sizeof(tosWord));
          }
        }

        if (ret != 0) {
          LOG(ERROR) << "Unable to set TOS for accepted socket "
                     << clientSocket;
        }
      } else {
        LOG(ERROR) << "Unable to get SYN packet for accepted socket "
                   << clientSocket;
      }
    }

    std::chrono::time_point<std::chrono::steady_clock> nowMs =
        std::chrono::steady_clock::now();
    auto timeSinceLastAccept = std::max<int64_t>(
        0,
        nowMs.time_since_epoch().count() -
            lastAccepTimestamp_.time_since_epoch().count());
    lastAccepTimestamp_ = nowMs;
    if (acceptRate_ < 1) {
      acceptRate_ *= 1 + acceptRateAdjustSpeed_ * timeSinceLastAccept;
      if (acceptRate_ >= 1) {
        acceptRate_ = 1;
      } else if (rand() > acceptRate_ * RAND_MAX) {
        ++numDroppedConnections_;
        if (clientSocket != NetworkSocket()) {
          closeNoInt(clientSocket);
          if (connectionEventCallback_) {
            connectionEventCallback_->onConnectionDropped(
                clientSocket,
                address,
                fmt::format(
                    "Server is rate limiting new connections. Current accept rate is {}",
                    acceptRate_));
          }
        }
        continue;
      }
    }

    if (clientSocket == NetworkSocket()) {
      if (errno == EAGAIN) {
        // No more sockets to accept right now.
        // Check for this code first, since it's the most common.
        return;
      } else if (errno == EMFILE || errno == ENFILE) {
        // We're out of file descriptors.  Perhaps we're accepting connections
        // too quickly. Pause accepting briefly to back off and give the server
        // a chance to recover.
        LOG(ERROR) << "accept failed: out of file descriptors; entering accept "
                      "back-off state";
        enterBackoff();

        // Dispatch the error message
        dispatchError("accept() failed", errno);
      } else {
        dispatchError("accept() failed", errno);
      }
      if (connectionEventCallback_) {
        connectionEventCallback_->onConnectionAcceptError(errno);
      }
      return;
    }

#if !FOLLY_HAVE_ACCEPT4
    // Explicitly set the new connection to non-blocking mode
    if (netops::set_socket_non_blocking(clientSocket) != 0) {
      closeNoInt(clientSocket);
      std::string errorMsg =
          "Failed to set accepted socket to non-blocking mode.";
      dispatchError(errorMsg.c_str(), errno);
      if (connectionEventCallback_) {
        connectionEventCallback_->onConnectionDropped(
            clientSocket,
            address,
            fmt::format("{} errno ({})", std::move(errorMsg), errno));
      }
      return;
    }
#endif

    // Inform the callback about the new connection
    dispatchSocket(clientSocket, std::move(address));

    // If we aren't accepting any more, break out of the loop
    if (!accepting_ || callbacks_.empty()) {
      break;
    }
  }
}

void AsyncServerSocket::dispatchSocket(
    NetworkSocket socket, SocketAddress&& address) {
  uint32_t startingIndex = callbackIndex_;

  auto timeBeforeEnqueue = std::chrono::steady_clock::now();

  // Short circuit if the callback is in the primary EventBase thread

  CallbackInfo* info = nextCallback(socket);
  if (info->eventBase == nullptr || info->eventBase == this->eventBase_) {
    info->callback->connectionAccepted(socket, address, {timeBeforeEnqueue});
    return;
  }

  const SocketAddress addr(address);
  // Create a message to send over the notification queue
  auto queueTimeout = *queueTimeout_;
  std::chrono::steady_clock::time_point deadline;
  if (queueTimeout.count() != 0) {
    deadline = timeBeforeEnqueue + queueTimeout;
  }

  NewConnMessage msg{socket, std::move(address), deadline, timeBeforeEnqueue};

  // Loop until we find a free queue to write to
  while (true) {
    if (info->consumer->getQueue().tryPutMessage(
            std::move(msg), maxNumMsgsInQueue_)) {
      if (connectionEventCallback_) {
        connectionEventCallback_->onConnectionEnqueuedForAcceptorCallback(
            socket, addr);
      }
      // Success! return.
      return;
    }

    // We couldn't add to queue.  Fall through to below

    if (acceptRateAdjustSpeed_ > 0) {
      // aggressively decrease accept rate when in trouble
      static const double kAcceptRateDecreaseSpeed = 0.1;
      acceptRate_ *= 1 - kAcceptRateDecreaseSpeed;
    }

    if (callbackIndex_ == startingIndex) {
      // The notification queue was full
      // We can't really do anything at this point other than close the socket.
      //
      // This should only happen if a user's service is behaving extremely
      // badly and none of the EventBase threads are looping fast enough to
      // process the incoming connections.  If the service is overloaded, it
      // should use pauseAccepting() to temporarily back off accepting new
      // connections, before they reach the point where their threads can't
      // even accept new messages.
      ++numDroppedConnections_;
      std::string errorMsg =
          "Failed to dispatch newly accepted socket: all accept callback queues are full";
      FB_LOG_EVERY_MS(ERROR, 1000) << errorMsg;
      closeNoInt(socket);
      if (connectionEventCallback_) {
        connectionEventCallback_->onConnectionDropped(socket, addr, errorMsg);
      }
      return;
    }

    info = nextCallback(socket);
  }
}

void AsyncServerSocket::dispatchError(const char* msgstr, int errnoValue) {
  uint32_t startingIndex = callbackIndex_;
  CallbackInfo* info = nextCallback();

  // Create a message to send over the notification queue
  ErrorMessage msg{errnoValue, msgstr};

  while (true) {
    // Short circuit if the callback is in the primary EventBase thread
    if (info->eventBase == nullptr || info->eventBase == this->eventBase_) {
      auto ex = make_exception_wrapper<std::runtime_error>(
          std::string(msgstr) + folly::to<std::string>(errnoValue));
      info->callback->acceptError(std::move(ex));
      return;
    }

    if (info->consumer->getQueue().tryPutMessage(
            std::move(msg), maxNumMsgsInQueue_)) {
      return;
    }
    // Fall through and try another callback

    if (callbackIndex_ == startingIndex) {
      // The notification queues for all of the callbacks were full.
      // We can't really do anything at this point.
      FB_LOG_EVERY_MS(ERROR, 1000)
          << "failed to dispatch accept error: all accept"
          << " callback queues are full: error msg:  " << msg.msg << ": "
          << errnoValue;
      return;
    }
    info = nextCallback();
  }
}

void AsyncServerSocket::enterBackoff() {
  // If this is the first time we have entered the backoff state,
  // allocate backoffTimeout_.
  if (backoffTimeout_ == nullptr) {
    try {
      backoffTimeout_ = new BackoffTimeout(this);
    } catch (const std::bad_alloc&) {
      // Man, we couldn't even allocate the timer to re-enable accepts.
      // We must be in pretty bad shape.  Don't pause accepting for now,
      // since we won't be able to re-enable ourselves later.
      LOG(ERROR) << "failed to allocate AsyncServerSocket backoff"
                 << " timer; unable to temporarly pause accepting";
      if (connectionEventCallback_) {
        connectionEventCallback_->onBackoffError();
      }
      return;
    }
  }

  // For now, we simply pause accepting for 1 second.
  //
  // We could add some smarter backoff calculation here in the future.  (e.g.,
  // start sleeping for longer if we keep hitting the backoff frequently.)
  // Typically the user needs to figure out why the server is overloaded and
  // fix it in some other way, though.  The backoff timer is just a simple
  // mechanism to try and give the connection processing code a little bit of
  // breathing room to catch up, and to avoid just spinning and failing to
  // accept over and over again.
  const uint32_t timeoutMS = 1000;
  if (!backoffTimeout_->scheduleTimeout(timeoutMS)) {
    LOG(ERROR) << "failed to schedule AsyncServerSocket backoff timer;"
               << "unable to temporarly pause accepting";
    if (connectionEventCallback_) {
      connectionEventCallback_->onBackoffError();
    }
    return;
  }

  // The backoff timer is scheduled to re-enable accepts.
  // Go ahead and disable accepts for now.  We leave accepting_ set to true,
  // since that tracks the desired state requested by the user.
  for (auto& handler : sockets_) {
    handler.unregisterHandler();
  }
  if (connectionEventCallback_) {
    connectionEventCallback_->onBackoffStarted();
  }
}

void AsyncServerSocket::backoffTimeoutExpired() {
  // accepting_ should still be true.
  // If pauseAccepting() was called while in the backoff state it will cancel
  // the backoff timeout.
  assert(accepting_);
  // We can't be detached from the EventBase without being paused
  assert(eventBase_ != nullptr);
  eventBase_->dcheckIsInEventBaseThread();

  // If all of the callbacks were removed, we shouldn't re-enable accepts
  if (callbacks_.empty()) {
    if (connectionEventCallback_) {
      connectionEventCallback_->onBackoffEnded();
    }
    return;
  }

  // Register the handler.
  for (auto& handler : sockets_) {
    if (!handler.registerHandler(EventHandler::READ | EventHandler::PERSIST)) {
      // We're hosed.  We could just re-schedule backoffTimeout_ to
      // re-try again after a little bit.  However, we don't want to
      // loop retrying forever if we can't re-enable accepts.  Just
      // abort the entire program in this state; things are really bad
      // and restarting the entire server is probably the best remedy.
      LOG(ERROR)
          << "failed to re-enable AsyncServerSocket accepts after backoff; "
          << "crashing now";
      abort();
    }
  }
  if (connectionEventCallback_) {
    connectionEventCallback_->onBackoffEnded();
  }
}

} // namespace folly
