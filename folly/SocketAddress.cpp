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

#include <folly/SocketAddress.h>

#include <bit>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include <fmt/core.h>

#include <folly/Exception.h>
#include <folly/hash/Hash.h>
#include <folly/net/NetOps.h>
#include <folly/net/NetworkSocket.h>

namespace {

/**
 * Mixes an unsigned integer into seed exactly as boost::hash_combine does in
 * Boost 1.77 (a MurmurHash2 or MurmurHash3 step), since callers may rely on
 * the values of SocketAddress::hash().
 */
template <typename T>
void hashCombine(size_t& seed, T value) {
  static_assert(std::is_unsigned_v<T>);
  size_t k = static_cast<size_t>(value);
  if constexpr (sizeof(T) > sizeof(size_t)) {
    k = 0;
    for (auto shift = sizeof(T) * 8 - sizeof(size_t) * 8; shift > 0;
         shift -= sizeof(size_t) * 8) {
      k ^= static_cast<size_t>(value >> shift) + (k << 6) + (k >> 2);
    }
    k ^= static_cast<size_t>(value) + (k << 6) + (k >> 2);
  }
  if constexpr (sizeof(size_t) == 8) {
    constexpr uint64_t m = 0xc6a4a7935bd1e995;
    k *= m;
    k ^= k >> 47;
    k *= m;
    seed ^= k;
    seed *= m;
    seed += 0xe6546b64;
  } else {
    k *= 0xcc9e2d51;
    k = std::rotl(k, 15);
    k *= 0x1b873593;
    seed ^= k;
    seed = std::rotl(seed, 13);
    seed = seed * 5 + 0xe6546b64;
  }
}

/**
 * A structure to free a struct addrinfo when it goes out of scope.
 */
struct ScopedAddrInfo {
  explicit ScopedAddrInfo(struct addrinfo* addrinfo) : info(addrinfo) {}
  ~ScopedAddrInfo() { freeaddrinfo(info); }

  struct addrinfo* info;
};

/**
 * A simple data structure for parsing a host-and-port string.
 *
 * Accepts a string of the form "<host>:<port>" or just "<port>",
 * and contains two string pointers to the host and the port portion of the
 * string.
 *
 * The HostAndPort may contain pointers into the original string.  It is
 * responsible for the user to ensure that the input string is valid for the
 * lifetime of the HostAndPort structure.
 */
struct HostAndPort {
  HostAndPort(const char* str, bool hostRequired)
      : host(nullptr), port(nullptr), allocated(nullptr) {
    // Look for the last colon
    const char* colon = strrchr(str, ':');
    if (colon == nullptr) {
      // No colon, just a port number.
      if (hostRequired) {
        throw std::invalid_argument(
            "expected a host and port string of the "
            "form \"<host>:<port>\"");
      }
      port = str;
      return;
    }

    // We have to make a copy of the string so we can modify it
    // and change the colon to a NUL terminator.
    allocated = strdup(str);
    if (!allocated) {
      throw std::bad_alloc();
    }

    char* allocatedColon = allocated + (colon - str);
    *allocatedColon = '\0';
    host = allocated;
    port = allocatedColon + 1;
    // bracketed IPv6 address, remove the brackets
    // allocatedColon[-1] is fine, as allocatedColon >= host and
    // *allocatedColon != *host therefore allocatedColon > host
    if (*host == '[' && allocatedColon[-1] == ']') {
      allocatedColon[-1] = '\0';
      ++host;
    }
  }

  ~HostAndPort() { free(allocated); }

  const char* host;
  const char* port;
  char* allocated;
};

struct GetAddrInfoError {
#ifdef _WIN32
  std::string error;
  const char* str() const { return error.c_str(); }
  explicit GetAddrInfoError(int errorCode) {
    auto s = gai_strerror(errorCode);
    using Char = std::remove_reference_t<decltype(*s)>;
    error.assign(s, s + std::char_traits<Char>::length(s));
  }
#else
  const char* error;
  const char* str() const { return error ? error : "Unknown error"; }
  explicit GetAddrInfoError(int errorCode) : error(gai_strerror(errorCode)) {}
#endif
};

} // namespace

namespace folly {

bool SocketAddress::isPrivateAddress() const {
  if (holdsInet()) {
    return std::get<IPAddr>(storage_).ip.isPrivate();
  } else {
    // Unix and vsock addresses are always local to a host.  Return true,
    // since this conforms to the semantics of returning true for IP loopback
    // addresses.
    return true;
  }
}

bool SocketAddress::isLoopbackAddress() const {
  if (holdsInet()) {
    return std::get<IPAddr>(storage_).ip.isLoopback();
#if FOLLY_HAVE_VSOCK
  } else if (holdsVsock()) {
    // VSOCK addresses with CID_LOCAL are considered loopback
    const auto& vsockAddr = std::get<VsockAddr>(storage_);
    return vsockAddr.cid == VMADDR_CID_LOCAL;
#endif
  } else {
    // Return true for UNIX addresses, since they are always local to a host.
    return true;
  }
}

void SocketAddress::setFromHostPort(const char* host, uint16_t port) {
  ScopedAddrInfo results(getAddrInfo(host, port, 0));
  setFromAddrInfo(results.info);
}

void SocketAddress::setFromIpPort(const char* ip, uint16_t port) {
  ScopedAddrInfo results(getAddrInfo(ip, port, AI_NUMERICHOST));
  setFromAddrInfo(results.info);
}

void SocketAddress::setFromIpAddrPort(const IPAddress& ipAddr, uint16_t port) {
  storage_ = IPAddr(ipAddr, port);
}

void SocketAddress::setFromLocalPort(uint16_t port) {
  ScopedAddrInfo results(getAddrInfo(nullptr, port, AI_ADDRCONFIG));
  setFromLocalAddr(results.info);
}

void SocketAddress::setFromLocalPort(const char* port) {
  ScopedAddrInfo results(getAddrInfo(nullptr, port, AI_ADDRCONFIG));
  setFromLocalAddr(results.info);
}

void SocketAddress::setFromLocalIpPort(const char* addressAndPort) {
  HostAndPort hp(addressAndPort, false);
  ScopedAddrInfo results(
      getAddrInfo(hp.host, hp.port, AI_NUMERICHOST | AI_ADDRCONFIG));
  setFromLocalAddr(results.info);
}

void SocketAddress::setFromIpPort(const char* addressAndPort) {
  HostAndPort hp(addressAndPort, true);
  ScopedAddrInfo results(getAddrInfo(hp.host, hp.port, AI_NUMERICHOST));
  setFromAddrInfo(results.info);
}

void SocketAddress::setFromHostPort(const char* hostAndPort) {
  HostAndPort hp(hostAndPort, true);
  ScopedAddrInfo results(getAddrInfo(hp.host, hp.port, 0));
  setFromAddrInfo(results.info);
}

#if FOLLY_HAVE_VSOCK
void SocketAddress::setFromVsockCIDPort(uint32_t cid, uint32_t port) {
  storage_ = VsockAddr(cid, port);
}
#endif

int SocketAddress::getPortFrom(const struct sockaddr* address) {
  switch (address->sa_family) {
    case AF_INET:
      return ntohs(((sockaddr_in*)address)->sin_port);

    case AF_INET6:
      return ntohs(((sockaddr_in6*)address)->sin6_port);

#if FOLLY_HAVE_VSOCK
    case AF_VSOCK:
      return ((sockaddr_vm*)address)->svm_port;
#endif

    default:
      return -1;
  }
}

const char* SocketAddress::getFamilyNameFrom(
    const struct sockaddr* address, const char* defaultResult) {
#define GETFAMILYNAMEFROM_IMPL(Family) \
  case Family:                         \
    return #Family

  switch ((int)address->sa_family) {
    GETFAMILYNAMEFROM_IMPL(AF_INET);
    GETFAMILYNAMEFROM_IMPL(AF_INET6);
    GETFAMILYNAMEFROM_IMPL(AF_UNIX);
#if FOLLY_HAVE_VSOCK
    GETFAMILYNAMEFROM_IMPL(AF_VSOCK);
#endif
    GETFAMILYNAMEFROM_IMPL(AF_UNSPEC);

    default:
      return defaultResult;
  }

#undef GETFAMILYNAMEFROM_IMPL
}

void SocketAddress::setFromPath(StringPiece path) {
  // Before we touch storage_, check to see if the length is too big.
  if (path.size() > sizeof(ExternalUnixAddr().addr->sun_path)) {
    throw std::invalid_argument(
        "socket path too large to fit into sockaddr_un");
  }

  // Create a new ExternalUnixAddr if we don't already have one
  if (!holdsUnix()) {
    storage_ = ExternalUnixAddr();
  }

  auto& unixAddr = std::get<ExternalUnixAddr>(storage_);
  size_t len = path.size();
  unixAddr.len = socklen_t(offsetof(struct sockaddr_un, sun_path) + len);
  memcpy(unixAddr.addr->sun_path, path.data(), len);
  // If there is room, put a terminating NUL byte in sun_path.  In general the
  // path should be NUL terminated, although getsockname() and getpeername()
  // may return Unix socket addresses with paths that fit exactly in sun_path
  // with no terminating NUL.
  if (len < sizeof(unixAddr.addr->sun_path)) {
    unixAddr.addr->sun_path[len] = '\0';
  }
}

void SocketAddress::setFromPeerAddress(NetworkSocket socket) {
  setFromSocket(socket, netops::getpeername);
}

void SocketAddress::setFromLocalAddress(NetworkSocket socket) {
  setFromSocket(socket, netops::getsockname);
}

void SocketAddress::setFromSockaddr(const struct sockaddr* address) {
  uint16_t port;

  if (address->sa_family == AF_INET) {
    port = ntohs(((sockaddr_in*)address)->sin_port);
  } else if (address->sa_family == AF_INET6) {
    port = ntohs(((sockaddr_in6*)address)->sin6_port);
  } else if (address->sa_family == AF_UNIX) {
    // We need an explicitly specified length for AF_UNIX addresses,
    // to be able to distinguish anonymous addresses from addresses
    // in Linux's abstract namespace.
    throw std::invalid_argument(
        "SocketAddress::setFromSockaddr(): the address "
        "length must be explicitly specified when "
        "setting AF_UNIX addresses");
#if FOLLY_HAVE_VSOCK
  } else if (address->sa_family == AF_VSOCK) {
    // For VSOCK addresses, store the CID and port in the VsockAddr
    const auto* vsockAddr = reinterpret_cast<const sockaddr_vm*>(address);
    storage_ = VsockAddr(vsockAddr->svm_cid, vsockAddr->svm_port);
    return;
#endif
  } else {
    throw std::invalid_argument(
        fmt::format(
            "SocketAddress::setFromSockaddr() called "
            "with unsupported address type {}",
            address->sa_family));
  }

  // For IP addresses, use the IPAddress constructor
  storage_ = IPAddr(folly::IPAddress(address), port);
}

void SocketAddress::setFromSockaddr(
    const struct sockaddr* address, socklen_t addrlen) {
  // Check the length to make sure we can access address->sa_family
  if (std::cmp_less(
          addrlen,
          offsetof(struct sockaddr, sa_family) + sizeof(address->sa_family))) {
    throw std::invalid_argument(
        "SocketAddress::setFromSockaddr() called "
        "with length too short for a sockaddr");
  }

  if (address->sa_family == AF_INET) {
    if (std::cmp_less(addrlen, sizeof(struct sockaddr_in))) {
      throw std::invalid_argument(
          "SocketAddress::setFromSockaddr() called "
          "with length too short for a sockaddr_in");
    }
    setFromSockaddr(reinterpret_cast<const struct sockaddr_in*>(address));
  } else if (address->sa_family == AF_INET6) {
    if (std::cmp_less(addrlen, sizeof(struct sockaddr_in6))) {
      throw std::invalid_argument(
          "SocketAddress::setFromSockaddr() called "
          "with length too short for a sockaddr_in6");
    }
    setFromSockaddr(reinterpret_cast<const struct sockaddr_in6*>(address));
  } else if (address->sa_family == AF_UNIX) {
    setFromSockaddr(
        reinterpret_cast<const struct sockaddr_un*>(address), addrlen);
#if FOLLY_HAVE_VSOCK
  } else if (address->sa_family == AF_VSOCK) {
    setFromSockaddr(reinterpret_cast<const struct sockaddr_vm*>(address));
#endif
  } else {
    throw std::invalid_argument(
        "SocketAddress::setFromSockaddr() called "
        "with unsupported address type");
  }
}

void SocketAddress::setFromSockaddr(const struct sockaddr_in* address) {
  assert(address->sin_family == AF_INET);
  setFromSockaddr((sockaddr*)address);
}

void SocketAddress::setFromSockaddr(const struct sockaddr_in6* address) {
  assert(address->sin6_family == AF_INET6);
  setFromSockaddr((sockaddr*)address);
}

void SocketAddress::setFromSockaddr(
    const struct sockaddr_un* address, socklen_t addrlen) {
  assert(address->sun_family == AF_UNIX);
  if (std::cmp_less(addrlen, offsetof(struct sockaddr_un, sun_path))) {
    throw std::invalid_argument(
        "SocketAddress: attempted to set a Unix socket "
        "with a length too short for a sockaddr_un");
  }
  if (std::cmp_greater(addrlen, sizeof(struct sockaddr_un))) {
    throw std::invalid_argument(
        "SocketAddress::setFromSockaddr() called "
        "with length too long for a sockaddr_un");
  }

  // Create a new ExternalUnixAddr if we don't already have one
  if (!holdsUnix()) {
    storage_ = ExternalUnixAddr();
  }

  auto& unixAddr = std::get<ExternalUnixAddr>(storage_);
  memcpy(unixAddr.addr, address, size_t(addrlen));
  updateUnixAddressLength(addrlen);

  // Fill the rest with 0s, just for safety
  if (std::cmp_less(addrlen, sizeof(struct sockaddr_un))) {
    auto p = reinterpret_cast<char*>(unixAddr.addr);
    memset(p + addrlen, 0, sizeof(struct sockaddr_un) - addrlen);
  }
}

#if FOLLY_HAVE_VSOCK
void SocketAddress::setFromSockaddr(const struct sockaddr_vm* address) {
  assert(address->svm_family == AF_VSOCK);
  storage_ = VsockAddr(address->svm_cid, address->svm_port);
}
#endif

const folly::IPAddress& SocketAddress::getIPAddress() const {
  auto family = getFamily();
  if (family != AF_INET && family != AF_INET6) {
    throw InvalidAddressFamilyException(family);
  }
  return std::get<IPAddr>(storage_).ip;
}

socklen_t SocketAddress::getActualSize() const {
  switch (getFamily()) {
    case AF_UNSPEC:
    case AF_INET:
      return sizeof(struct sockaddr_in);
    case AF_INET6:
      return sizeof(struct sockaddr_in6);
    case AF_UNIX:
      return std::get<ExternalUnixAddr>(storage_).len;
#if FOLLY_HAVE_VSOCK
    case AF_VSOCK:
      return sizeof(struct sockaddr_vm);
#endif
    default:
      throw std::invalid_argument(
          "SocketAddress::getActualSize() called "
          "with unrecognized address family");
  }
}

std::string SocketAddress::getFullyQualified() const {
  if (!isFamilyInet()) {
    throw std::invalid_argument("Can't get address str for non ip address");
  }
  return std::get<IPAddr>(storage_).ip.toFullyQualified();
}

std::string SocketAddress::getAddressStr() const {
  if (!isFamilyInet()) {
    throw std::invalid_argument("Can't get address str for non ip address");
  }
  return std::get<IPAddr>(storage_).ip.str();
}

bool SocketAddress::isFamilyInet() const {
  auto family = getFamily();
  return family == AF_INET || family == AF_INET6;
}

void SocketAddress::getAddressStr(char* buf, size_t buflen) const {
  if (buflen == 0) {
    return;
  }
  auto ret = getAddressStr();
  size_t len = std::min(buflen - 1, ret.size());
  memcpy(buf, ret.data(), len);
  buf[len] = '\0';
}

uint16_t SocketAddress::getPort() const {
  switch (getFamily()) {
    case AF_INET:
    case AF_INET6:
      return std::get<IPAddr>(storage_).port;
    default:
      throw std::invalid_argument(
          "SocketAddress::getPort() called on non-IP "
          "address");
  }
}

#if FOLLY_HAVE_VSOCK
uint32_t SocketAddress::getVsockPort() const {
  switch (getFamily()) {
    case AF_VSOCK:
      return std::get<VsockAddr>(storage_).port;
    default:
      throw std::invalid_argument(
          "SocketAddress::getVsockPort() called on non-VSOCK address");
  }
}
#endif

void SocketAddress::setPort(uint16_t port) {
  switch (getFamily()) {
    case AF_INET:
    case AF_INET6:
      std::get<IPAddr>(storage_).port = port;
      return;
    default:
      throw std::invalid_argument(
          "SocketAddress::setPort() called on non-IP "
          "address");
  }
}

void SocketAddress::convertToIPv4() {
  if (!tryConvertToIPv4()) {
    throw std::invalid_argument(
        "convertToIPv4() called on an address that is "
        "not an IPv4-mapped address");
  }
}

bool SocketAddress::tryConvertToIPv4() {
  if (!isIPv4Mapped()) {
    return false;
  }

  auto& ipAddr = std::get<IPAddr>(storage_);
  ipAddr.ip = folly::IPAddress::createIPv4(ipAddr.ip);
  return true;
}

bool SocketAddress::mapToIPv6() {
  if (getFamily() != AF_INET) {
    return false;
  }

  auto& ipAddr = std::get<IPAddr>(storage_);
  ipAddr.ip = folly::IPAddress::createIPv6(ipAddr.ip);
  return true;
}

std::string SocketAddress::getHostStr() const {
  return getIpString(0);
}

std::string SocketAddress::getPath() const {
  if (!holdsUnix()) {
    throw std::invalid_argument(
        "SocketAddress: attempting to get path "
        "for a non-Unix address");
  }

  const auto& unixAddr = std::get<ExternalUnixAddr>(storage_);
  if (unixAddr.pathLength() == 0) {
    // anonymous address
    return std::string();
  }
  if (unixAddr.addr->sun_path[0] == '\0') {
    // abstract namespace
    return std::string(unixAddr.addr->sun_path, size_t(unixAddr.pathLength()));
  }

  return std::string(
      unixAddr.addr->sun_path,
      strnlen(unixAddr.addr->sun_path, size_t(unixAddr.pathLength())));
}

#if FOLLY_HAVE_VSOCK
uint32_t SocketAddress::getVsockCID() const {
  if (!holdsVsock()) {
    throw std::invalid_argument(
        "SocketAddress: attempting to get CID "
        "for a non-VSOCK address");
  }

  return std::get<VsockAddr>(storage_).cid;
}
#endif

std::string SocketAddress::describe() const {
  if (holdsUnix()) {
    const auto& unixAddr = std::get<ExternalUnixAddr>(storage_);
    if (unixAddr.pathLength() == 0) {
      return "<anonymous unix address>";
    }

    if (unixAddr.addr->sun_path[0] == '\0') {
      // Linux supports an abstract namespace for unix socket addresses
      return "<abstract unix address>";
    }

    return std::string(
        unixAddr.addr->sun_path,
        strnlen(unixAddr.addr->sun_path, size_t(unixAddr.pathLength())));
  }
  switch (getFamily()) {
    case AF_UNSPEC:
      return "<uninitialized address>";
    case AF_INET: {
      char buf[NI_MAXHOST + 16];
      getAddressStr(buf, sizeof(buf));
      size_t iplen = strlen(buf);
      snprintf(buf + iplen, sizeof(buf) - iplen, ":%" PRIu16, getPort());
      return buf;
    }
    case AF_INET6: {
      char buf[NI_MAXHOST + 18];
      buf[0] = '[';
      getAddressStr(buf + 1, sizeof(buf) - 1);
      size_t iplen = strlen(buf);
      snprintf(buf + iplen, sizeof(buf) - iplen, "]:%" PRIu16, getPort());
      return buf;
    }
#if FOLLY_HAVE_VSOCK
    case AF_VSOCK: {
      char buf[32];
      const auto& vsockAddr = std::get<VsockAddr>(storage_);
      auto* maybeName = vsockAddr.getMappedName();
      if (maybeName) {
        snprintf(
            buf, sizeof(buf), "[%s:%" PRIu32 "]", maybeName, vsockAddr.port);
      } else {
        snprintf(
            buf,
            sizeof(buf),
            "[%" PRIu32 ":%" PRIu32 "]",
            vsockAddr.cid,
            vsockAddr.port);
      }
      return buf;
    }
#endif
    default: {
      char buf[64];
      snprintf(buf, sizeof(buf), "<unknown address family %d>", getFamily());
      return buf;
    }
  }
}

bool SocketAddress::operator==(const SocketAddress& other) const {
  if (other.getFamily() != getFamily()) {
    return false;
  }

  if (holdsUnix()) {
    const auto& thisUnixAddr = std::get<ExternalUnixAddr>(storage_);
    const auto& otherUnixAddr = std::get<ExternalUnixAddr>(other.storage_);

    // anonymous addresses are never equal to any other addresses
    if (thisUnixAddr.pathLength() == 0 || otherUnixAddr.pathLength() == 0) {
      return false;
    }

    if (thisUnixAddr.len != otherUnixAddr.len) {
      return false;
    }
    int cmp = memcmp(
        thisUnixAddr.addr->sun_path,
        otherUnixAddr.addr->sun_path,
        size_t(thisUnixAddr.pathLength()));
    return cmp == 0;
  }

  switch (getFamily()) {
    case AF_INET:
    case AF_INET6:
      return (std::get<IPAddr>(other.storage_).ip ==
              std::get<IPAddr>(storage_).ip) &&
          (std::get<IPAddr>(other.storage_).port ==
           std::get<IPAddr>(storage_).port);
#if FOLLY_HAVE_VSOCK
    case AF_VSOCK:
      return (std::get<VsockAddr>(other.storage_).cid ==
              std::get<VsockAddr>(storage_).cid) &&
          (std::get<VsockAddr>(other.storage_).port ==
           std::get<VsockAddr>(storage_).port);
#endif
    case AF_UNSPEC:
      return std::get<IPAddr>(other.storage_).ip.empty();
    default:
      throw_exception<std::invalid_argument>(
          "SocketAddress: unsupported address family for comparison");
  }
}

bool SocketAddress::prefixMatch(
    const SocketAddress& other, unsigned prefixLength) const {
  if (other.getFamily() != getFamily()) {
    return false;
  }
  uint8_t mask_length = 128;
  switch (getFamily()) {
    case AF_INET:
      mask_length = 32;
      [[fallthrough]];
    case AF_INET6: {
      auto prefix = folly::IPAddress::longestCommonPrefix(
          {std::get<IPAddr>(storage_).ip, mask_length},
          {std::get<IPAddr>(other.storage_).ip, mask_length});
      return prefix.second >= prefixLength;
    }
    default:
      return false;
  }
}

size_t SocketAddress::hash() const {
  size_t seed = folly::hash::twang_mix64(getFamily());

  if (holdsUnix()) {
    const auto& unixAddr = std::get<ExternalUnixAddr>(storage_);
    enum { kUnixPathMax = sizeof(unixAddr.addr->sun_path) };
    const char* path = unixAddr.addr->sun_path;
    auto pathLength = unixAddr.pathLength();
    // TODO: this probably could be made more efficient
    for (off_t n = 0; n < pathLength; ++n) {
      hashCombine(seed, folly::hash::twang_mix64(uint64_t(path[n])));
    }
  }

  switch ((int)getFamily()) {
    case AF_INET:
    case AF_INET6: {
      hashCombine(seed, std::get<IPAddr>(storage_).port);
      hashCombine(seed, std::get<IPAddr>(storage_).ip.hash());
      break;
    }
#if FOLLY_HAVE_VSOCK
    case AF_VSOCK: {
      hashCombine(seed, std::get<VsockAddr>(storage_).port);
      hashCombine(seed, std::get<VsockAddr>(storage_).cid);
      break;
    }
#endif
    case AF_UNIX:
      // Already handled above
      break;
    case AF_UNSPEC:
      hashCombine(seed, std::get<IPAddr>(storage_).ip.hash());
      break;
    default:
      throw_exception<std::invalid_argument>(
          "SocketAddress: unsupported address family for comparison");
  }

  return seed;
}

struct addrinfo* SocketAddress::getAddrInfo(
    const char* host, uint16_t port, int flags) {
  // getaddrinfo() requires the port number as a string
  char portString[sizeof("65535")];
  snprintf(portString, sizeof(portString), "%" PRIu16, port);

  return getAddrInfo(host, portString, flags);
}

struct addrinfo* SocketAddress::getAddrInfo(
    const char* host, const char* port, int flags) {
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV | flags;

  struct addrinfo* results;
  int error = getaddrinfo(host, port, &hints, &results);
  if (error != 0) {
    auto os = fmt::format(
        "Failed to resolve address for '{}': {} (error={})",
        (host ? host : "<null>"),
        GetAddrInfoError(error).str(),
        error);
    throw std::system_error(error, std::generic_category(), os);
  }

  return results;
}

void SocketAddress::setFromAddrInfo(const struct addrinfo* info) {
  setFromSockaddr(info->ai_addr, socklen_t(info->ai_addrlen));
}

void SocketAddress::setFromLocalAddr(const struct addrinfo* info) {
  // If an IPv6 address is present, prefer to use it, since IPv4 addresses
  // can be mapped into IPv6 space.
  for (const struct addrinfo* ai = info; ai != nullptr; ai = ai->ai_next) {
    if (ai->ai_family == AF_INET6) {
      setFromSockaddr(ai->ai_addr, socklen_t(ai->ai_addrlen));
      return;
    }
  }

  // Otherwise, just use the first address in the list.
  setFromSockaddr(info->ai_addr, socklen_t(info->ai_addrlen));
}

void SocketAddress::setFromSocket(
    NetworkSocket socket,
    int (*fn)(NetworkSocket, struct sockaddr*, socklen_t*)) {
  // Try to put the address into a local storage buffer.
  sockaddr_storage tmp_sock;
  socklen_t addrLen = sizeof(tmp_sock);
  if (fn(socket, (sockaddr*)&tmp_sock, &addrLen) != 0) {
    folly::throwSystemError("setFromSocket() failed");
  }

  setFromSockaddr((sockaddr*)&tmp_sock, addrLen);
}

std::string SocketAddress::getIpString(int flags) const {
  char addrString[NI_MAXHOST];
  getIpString(addrString, sizeof(addrString), flags);
  return std::string(addrString);
}

void SocketAddress::getIpString(char* buf, size_t buflen, int flags) const {
  auto family = getFamily();
  if (family != AF_INET && family != AF_INET6) {
    throw std::invalid_argument(
        "SocketAddress: attempting to get IP address "
        "for a non-IP address");
  }

  sockaddr_storage tmp_sock;
  std::get<IPAddr>(storage_).ip.toSockaddrStorage(
      &tmp_sock, std::get<IPAddr>(storage_).port);
  int rc = getnameinfo(
      (sockaddr*)&tmp_sock,
      sizeof(sockaddr_storage),
      buf,
      buflen,
      nullptr,
      0,
      flags);
  if (rc != 0) {
    auto os = fmt::format(
        "getnameinfo() failed in getIpString() error = {}",
        GetAddrInfoError(rc).str());
    throw std::system_error(rc, std::generic_category(), os);
  }
}

void SocketAddress::updateUnixAddressLength(socklen_t addrlen) {
  if (std::cmp_less(addrlen, offsetof(struct sockaddr_un, sun_path))) {
    throw std::invalid_argument(
        "SocketAddress: attempted to set a Unix socket "
        "with a length too short for a sockaddr_un");
  }

  auto& unixAddr = std::get<ExternalUnixAddr>(storage_);
  unixAddr.len = addrlen;
  if (unixAddr.pathLength() == 0) {
    // anonymous address
    return;
  }

  if (unixAddr.addr->sun_path[0] == '\0') {
    // abstract namespace.  honor the specified length
  } else {
    // Call strnlen(), just in case the length was overspecified.
    size_t maxLength = addrlen - offsetof(struct sockaddr_un, sun_path);
    size_t pathLength = strnlen(unixAddr.addr->sun_path, maxLength);
    unixAddr.len =
        socklen_t(offsetof(struct sockaddr_un, sun_path) + pathLength);
  }
}

bool SocketAddress::operator<(const SocketAddress& other) const {
  if (getFamily() != other.getFamily()) {
    return getFamily() < other.getFamily();
  }

  if (holdsUnix()) {
    // Anonymous addresses can't be compared to anything else.
    // Return that they are never less than anything.
    //
    // Note that this still meets the requirements for a strict weak
    // ordering, so we can use this operator<() with standard C++
    // containers.
    const auto& thisUnixAddr = std::get<ExternalUnixAddr>(storage_);
    auto thisPathLength = thisUnixAddr.pathLength();
    if (thisPathLength == 0) {
      return false;
    }
    const auto& otherUnixAddr = std::get<ExternalUnixAddr>(other.storage_);
    auto otherPathLength = otherUnixAddr.pathLength();
    if (otherPathLength == 0) {
      return true;
    }

    // Compare based on path length first, for efficiency
    if (thisPathLength != otherPathLength) {
      return thisPathLength < otherPathLength;
    }
    int cmp = memcmp(
        thisUnixAddr.addr->sun_path,
        otherUnixAddr.addr->sun_path,
        size_t(thisPathLength));
    return cmp < 0;
  }
  switch (getFamily()) {
    case AF_INET:
    case AF_INET6: {
      auto& thisAddr = std::get<IPAddr>(storage_);
      auto& otherAddr = std::get<IPAddr>(other.storage_);
      if (thisAddr.port != otherAddr.port) {
        return thisAddr.port < otherAddr.port;
      }

      return thisAddr.ip < otherAddr.ip;
    }
    case AF_UNSPEC:
    default:
      throw std::invalid_argument(
          "SocketAddress: unsupported address family for comparing");
  }
}

#if FOLLY_HAVE_VSOCK
const char* SocketAddress::VsockAddr::getMappedName() const {
  // Use special names for well-known CIDs
  if (cid == VMADDR_CID_ANY) {
    return "any";
  } else if (cid == VMADDR_CID_HYPERVISOR) {
    return "hypervisor";
  } else if (cid == VMADDR_CID_LOCAL) {
    return "local";
  } else if (cid == VMADDR_CID_HOST) {
    return "host";
  } else {
    return nullptr;
  }
}
#endif

size_t hash_value(const SocketAddress& address) {
  return address.hash();
}

std::ostream& operator<<(std::ostream& os, const SocketAddress& addr) {
  os << addr.describe();
  return os;
}

} // namespace folly
