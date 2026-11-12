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

#include <sys/types.h>

#include <cstddef>
#include <iosfwd>
#include <string>

#include <variant>
#include <folly/IPAddress.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/net/NetworkSocket.h>
#include <folly/portability/Config.h>
#include <folly/portability/Sockets.h>

#if FOLLY_HAVE_VSOCK
#include <linux/vm_sockets.h>
#endif

namespace folly {

/**
 * Provides a unified interface for socket addresses.
 *
 * @class folly::SocketAddress
 *
 */

class SocketAddress {
 public:
  SocketAddress() = default;

  /**
   * Construct a SocketAddress from a hostname and port.
   *
   * Note: If the host parameter is not a numeric IP address, hostname
   * resolution will be performed, which can be quite slow.
   *
   * Raises std::system_error on error.
   *
   * @param host The IP address (or hostname, if allowNameLookup is true)
   * @param port The port (in host byte order)
   * @param allowNameLookup  If true, attempt to perform hostname lookup
   *        if the hostname does not appear to be a numeric IP address.
   *        This is potentially a very slow operation, so is disabled by
   *        default.
   */
  SocketAddress(const char* host, uint16_t port, bool allowNameLookup = false) {
    // Initialize the address family first,
    // since setFromHostPort() and setFromIpPort() will check it.

    if (allowNameLookup) {
      setFromHostPort(host, port);
    } else {
      setFromIpPort(host, port);
    }
  }
  /**
   * Similar to the constructor which accepts hostname and port.
   * This variant accepts host as std::string.
   */
  SocketAddress(
      const std::string& host, uint16_t port, bool allowNameLookup = false) {
    // Initialize the address family first,
    // since setFromHostPort() and setFromIpPort() will check it.

    if (allowNameLookup) {
      setFromHostPort(host.c_str(), port);
    } else {
      setFromIpPort(host.c_str(), port);
    }
  }
  /**
   * Construct a SocketAddress from a hostname and port.
   *
   * Raises std::system_error on error.
   *
   * @param ipAddr The IP address
   * @param port The port (in host byte order)
   */
  SocketAddress(const IPAddress& ipAddr, uint16_t port) {
    setFromIpAddrPort(ipAddr, port);
  }

  SocketAddress(const SocketAddress& addr) { storage_ = addr.storage_; }

  SocketAddress& operator=(const SocketAddress& addr) {
    storage_ = addr.storage_;
    return *this;
  }

  SocketAddress(SocketAddress&& addr) noexcept {
    storage_ = std::move(addr.storage_);
  }

  SocketAddress& operator=(SocketAddress&& addr) {
    storage_ = std::move(addr.storage_);
    return *this;
  }

  ~SocketAddress() = default;

  /**
   * Return whether this SocketAddress is initialized.
   */
  bool isInitialized() const { return (getFamily() != AF_UNSPEC); }

  /**
   * Return whether this address is within private network.
   *
   * According to RFC1918, the 10/8 prefix, 172.16/12 prefix, and 192.168/16
   * prefix are reserved for private networks.
   * fc00::/7 is the IPv6 version, defined in RFC4139.  IPv6 link-local
   * addresses (fe80::/10) are also considered private addresses.
   *
   * The loopback addresses 127/8 and ::1 are also regarded as private networks
   * for the purpose of this function.
   *
   * @return true if this is a private network address, false otherwise
   */
  bool isPrivateAddress() const;

  /**
   * Return whether this address is a loopback address.
   *
   * @return true if this is a loopback address, false otherwise
   */
  bool isLoopbackAddress() const;

  /**
   * Reset this SocketAddress by clearing the associated address and
   * freeing up any external storage being used.
   */
  void reset() { storage_ = IPAddr(); }

  /**
   * @overloadbrief Initialize this SocketAddress from a hostname and port.
   *
   * Note: If the host parameter is not a numeric IP address, hostname
   * resolution will be performed, which can be quite slow.
   *
   * If the hostname resolves to multiple addresses, only the first will be
   * returned.
   *
   * Raises std::system_error on error.
   *
   * @param host The hostname or IP address
   * @param port The port (in host byte order)
   */
  void setFromHostPort(const char* host, uint16_t port);
  /**
   * Similar to the function setFromHostPort above, but accepts the IP address
   * as a std::string.
   */
  void setFromHostPort(const std::string& host, uint16_t port) {
    setFromHostPort(host.c_str(), port);
  }

  /**
   * @overloadbrief Initialize this SocketAddress from an IP address and port.
   *
   * This is similar to setFromHostPort(), but only accepts numeric IP
   * addresses.  If the IP string does not look like an IP address, it throws a
   * std::invalid_argument rather than trying to perform a hostname resolution.
   *
   * Raises std::system_error on error.
   *
   * @param ip The IP address, as a human-readable string.
   * @param port The port (in host byte order)
   */
  void setFromIpPort(const char* ip, uint16_t port);
  /**
   * Similar to the function setFromIpPort above, but accepts the IP address as
   * a std::string.
   */
  void setFromIpPort(const std::string& ip, uint16_t port) {
    setFromIpPort(ip.c_str(), port);
  }

  /**
   * Initialize this SocketAddress from an IPAddress struct and port.
   *
   * @param ip The IP address in IPAddress format
   * @param port The port (in host byte order)
   */
  void setFromIpAddrPort(const IPAddress& ip, uint16_t port);

  /**
   * @overloadbrief Initialize this SocketAddress from a local port number.
   *
   * This is intended to be used by server code to determine the address to
   * listen on.
   *
   * If the current machine has any IPv6 addresses configured, an IPv6 address
   * will be returned (since connections from IPv4 clients can be mapped to the
   * IPv6 address).  If the machine does not have any IPv6 addresses, an IPv4
   * address will be returned.
   *
   * @param port The local port number (in host byte order)
   */
  void setFromLocalPort(uint16_t port);
  /**
   * Initialize this SocketAddress from a local port number.
   *
   * This version of setFromLocalPort() accepts the port as a string.  A
   * std::invalid_argument will be raised if the string does not refer to a port
   * number.  Non-numeric service port names are not accepted.
   *
   * @param port The local port number
   */
  void setFromLocalPort(const char* port);
  /**
   * Similar to the function setFromLocalPort above, but accepts the port as
   * a std::string.
   */
  void setFromLocalPort(const std::string& port) {
    return setFromLocalPort(port.c_str());
  }

  /**
   * @overloadbrief Initialize this SocketAddress from a local port number and
   * optional IP address.
   *
   * The addressAndPort string may be specified either as "<ip>:<port>", or
   * just as "<port>".  If the IP is not specified, the address will be
   * initialized to 0, so that a server socket bound to this address will
   * accept connections on all local IP addresses.
   *
   * Both the IP address and port number must be numeric.  DNS host names and
   * non-numeric service port names are not accepted.
   *
   * @param addressAndPort Address and the port separated by ':', or the port
   */
  void setFromLocalIpPort(const char* addressAndPort);
  /**
   * Similar to the function setFromLocalIpPort above, but accepts the address
   * and port as a std::string.
   */
  void setFromLocalIpPort(const std::string& addressAndPort) {
    return setFromLocalIpPort(addressAndPort.c_str());
  }

  /**
   * @overloadbrief Initialize this SocketAddress from an IP address and port
   * number.
   *
   * The addressAndPort string must be of the form "<ip>:<port>".  E.g.,
   * "10.0.0.1:1234".
   *
   * Both the IP address and port number must be numeric.  DNS host names and
   * non-numeric service port names are not accepted.
   *
   * @param addressAndPort Address and the port separated by ':'
   */
  void setFromIpPort(const char* addressAndPort);
  /**
   * Similar to the function setFromIpPort above, but accepts the address
   * and port as a std::string.
   */
  void setFromIpPort(const std::string& addressAndPort) {
    return setFromIpPort(addressAndPort.c_str());
  }

  /**
   * @overloadbrief Initialize this SocketAddress from a host name and port
   * number.
   *
   * The addressAndPort string must be of the form "<host>:<port>".  E.g.,
   * "www.facebook.com:443".
   *
   * If the host name is not a numeric IP address, a DNS lookup will be
   * performed.  Beware that the DNS lookup may be very slow.  The port number
   * must be numeric; non-numeric service port names are not accepted.
   *
   * @param hostAndPort Host name and the port separated by ':'
   */
  void setFromHostPort(const char* hostAndPort);
  /**
   * Similar to the function setFromHostPort above, but accepts the host name
   * and port as a std::string.
   */
  void setFromHostPort(const std::string& hostAndPort) {
    return setFromHostPort(hostAndPort.c_str());
  }

#if FOLLY_HAVE_VSOCK
  /**
   * Initialize this SocketAddress from a VSOCK CID and port.
   */
  void setFromVsockCIDPort(uint32_t cid, uint32_t port);
#endif

  /**
   * Returns the port number from the given socketaddr structure.
   *
   * Currently only IPv4 and IPv6 are supported.
   *
   * @param address The socketaddr structure to get port from
   *
   * @return The port number, or -1 for unsupported socket families.
   */
  static int getPortFrom(const struct sockaddr* address);

  /**
   * Returns the family name from the given socketaddr structure (e.g.: AF_INET6
   * for IPv6).
   *
   * @param address The socketaddr structure to get family name from
   * @param defaultResult The default family name to be returned in case of
   * unsupported socket. If no value is passed, `nullptr` is returned as default
   * family name.
   *
   * @return The family name, or `defaultResult` passed for unsupported socket
   * families.
   */
  static const char* getFamilyNameFrom(
      const struct sockaddr* address, const char* defaultResult = nullptr);

  /**
   * @overloadbrief Initialize this SocketAddress from a local unix path.
   *
   * Raises std::invalid_argument on error.
   *
   * @param path Local unix path
   */
  void setFromPath(StringPiece path);
  /**
   * Similar to setFromPath, but accepts local unix path as const char* and
   * its length.
   */
  void setFromPath(const char* path, size_t length) {
    setFromPath(StringPiece{path, length});
  }

  /**
   * Construct a SocketAddress from a local unix socket path.
   *
   * Raises std::invalid_argument on error.
   *
   * @param path The Unix domain socket path.
   */
  static SocketAddress makeFromPath(StringPiece path) {
    SocketAddress addr;
    addr.setFromPath(path);
    return addr;
  }

  /**
   * Construct a SocketAddress from a local port number.
   * see setFromLocalPort for more details.
   *
   * Raises std::invalid_argument on error.
   *
   * @param port The local port number (in host byte order)
   */
  static SocketAddress makeFromLocalPort(uint16_t port) {
    SocketAddress addr;
    addr.setFromLocalPort(port);
    return addr;
  }

  /**
   * Similar to makeFromLocalPort(uint16_t port), but accepts the port as
   * a std::string.
   *
   * Raises std::invalid_argument on error.
   *
   * @param port The local port number
   */
  static SocketAddress makeFromLocalPort(const char* port) {
    SocketAddress addr;
    addr.setFromLocalPort(port);
    return addr;
  }

  /**
   * Initialize this SocketAddress from a socket's peer address.
   *
   * Raises std::system_error on error.
   *
   * @param socket Socket whose peer address is used to initialize
   */
  void setFromPeerAddress(NetworkSocket socket);

  /**
   * Initialize this SocketAddress from a socket's local address.
   *
   * Raises std::system_error on error.
   *
   * @param socket Socket whose local address is used to initialize
   */
  void setFromLocalAddress(NetworkSocket socket);

  /**
   * Initialize this folly::SocketAddress from a struct sockaddr.
   *
   * Raises std::system_error on error.
   *
   * This method is not supported for AF_UNIX addresses.  For unix addresses,
   * the address length must be explicitly specified.
   *
   * @param address  A struct sockaddr.  The size of the address is implied
   *                 from address->sa_family.
   */
  void setFromSockaddr(const struct sockaddr* address);

  /**
   * Initialize this SocketAddress from a struct sockaddr.
   *
   * Raises std::system_error on error.
   *
   * @param address  A struct sockaddr.
   * @param addrlen  The length of address data available.  This must be long
   *                 enough for the full address type required by
   *                 address->sa_family.
   */
  void setFromSockaddr(const struct sockaddr* address, socklen_t addrlen);

  /**
   * Initialize this SocketAddress from a struct sockaddr_in.
   *
   * @param address  A struct sockaddr_in to initialize from
   */
  void setFromSockaddr(const struct sockaddr_in* address);

  /**
   * Initialize this SocketAddress from a struct sockaddr_in6.
   *
   * @param address  A struct sockaddr_in6 to initialize from
   */
  void setFromSockaddr(const struct sockaddr_in6* address);

  /**
   * Initialize this SocketAddress from a struct sockaddr_un.
   *
   * Note that the addrlen parameter is necessary to properly detect anonymous
   * addresses, which have 0 valid path bytes, and may not even have a NUL
   * character at the start of the path.
   *
   * @param address  A struct sockaddr_un.
   * @param addrlen  The length of address data.  This should include all of
   *                 the valid bytes of sun_path, not including any NUL
   *                 terminator.
   */
  void setFromSockaddr(const struct sockaddr_un* address, socklen_t addrlen);

#if FOLLY_HAVE_VSOCK
  /**
   * Initialize this SocketAddress from a struct sockaddr_vm.
   *
   * @param address  A struct sockaddr_vm to initialize from
   */
  void setFromSockaddr(const struct sockaddr_vm* address);
#endif

  /**
   * Fill in a given sockaddr_storage with the ip or unix address.
   *
   * @param addr sockaddr_storage out parameter
   *
   * @return The actual size of the socket address
   */
  socklen_t getAddress(sockaddr_storage* addr) const {
    if (isFamilyInet()) {
      return std::get<IPAddr>(storage_).ip.toSockaddrStorage(
          addr, htons(std::get<IPAddr>(storage_).port));
#if FOLLY_HAVE_VSOCK
    } else if (holdsVsock()) {
      const auto& vsockAddr = std::get<VsockAddr>(storage_);
      auto* svm = reinterpret_cast<sockaddr_vm*>(addr);
      memset(svm, 0, sizeof(sockaddr_vm));
      svm->svm_family = AF_VSOCK;
      svm->svm_cid = vsockAddr.cid;
      svm->svm_port = vsockAddr.port;
      return sizeof(sockaddr_vm);
#endif
    } else {
      const auto& unixAddr = std::get<ExternalUnixAddr>(storage_);
      memcpy(addr, unixAddr.addr, sizeof(*unixAddr.addr));
      return unixAddr.len;
    }
  }

  /**
   * Return the IP address of this SocketAddress.
   *
   * @throws folly::InvalidAddressFamilyException if the family is not IPv4 or
   * IPv6
   *
   * @return IP address
   */
  const folly::IPAddress& getIPAddress() const;

  /**
   * DEPRECATED: SocketAddress::getAddress() above returns the same size as
   * getActualSize()
   *
   * Return the size of the underlying socket address
   *
   * @return The size of the socket address
   */
  socklen_t getActualSize() const;

  /**
   * Return the address family of this SocketAddress
   *
   * @return Socket address family
   */
  sa_family_t getFamily() const {
    if (holdsUnix()) {
      return sa_family_t(AF_UNIX);
#if FOLLY_HAVE_VSOCK
    } else if (holdsVsock()) {
      return sa_family_t(AF_VSOCK);
#endif
    } else {
      return std::get<IPAddr>(storage_).ip.family();
    }
  }

  /**
   * Return if the SocketAddress is `empty` i.e., the address family is
   * unspecified.
   *
   * @return true, if socket is `empty` i.e., address family is unspecified,
   * else false
   */
  bool empty() const { return getFamily() == AF_UNSPEC; }

  /**
   * Get a string representation of the IPv4 or IPv6 address.
   *
   * Raises std::invalid_argument if an error occurs (for example, if
   * the address is not an IPv4 or IPv6 address).
   *
   * @return String representation of the IP address
   */
  std::string getAddressStr() const;

  /**
   * Get a string representation of the IPv4 or IPv6 address.
   *
   * Raises std::invalid_argument if an error occurs (for example, if
   * the address is not an IPv4 or IPv6 address).
   *
   * @param buf Char buffer to write the string representation into
   * @param buflen Size of the buffer
   */
  void getAddressStr(char* buf, size_t buflen) const;

  /**
   * Return whether this address is a valid IPv4 or IPv6 address.
   *
   * @return true if address a valid IPv4 or IPv6 address, false otherwise
   */
  bool isFamilyInet() const;

  /**
   * For v4 & v6 addresses, return the fully qualified address string
   *
   * Raises std::invalid_argument if this is not an IPv4 or IPv6 address.
   *
   * @return Fully qualified IP address
   */
  std::string getFullyQualified() const;

  /**
   * Get the IPv4 or IPv6 port for this address.
   *
   * Raises std::invalid_argument if this is not an IPv4 or IPv6 address.
   *
   * @return The port, in host byte order
   */
  uint16_t getPort() const;

#if FOLLY_HAVE_VSOCK
  /**
   * Get the port for a VSOCK address.
   *
   * Raises std::invalid_argument if this is not a VSOCK address.
   *
   * @return The port, in host byte order
   */
  uint32_t getVsockPort() const;
#endif

  /**
   * Set the IPv4 or IPv6 port for this address.
   *
   * Raises std::invalid_argument if this is not an IPv4 or IPv6 address.
   *
   * @param port The port to set, in host byte order
   */
  void setPort(uint16_t port);

  /**
   * Return true if this is an IPv4-mapped IPv6 address.
   *
   * @return true if this address is a IPv6 address which is IPv4-mapped,
   * false otherwise
   */
  bool isIPv4Mapped() const {
    return (
        getFamily() == AF_INET6 &&
        std::get<IPAddr>(storage_).ip.isIPv4Mapped());
  }

  /**
   * Convert an IPv4-mapped IPv6 address to an IPv4 address.
   *
   * Raises std::invalid_argument if this is not an IPv4-mapped IPv6 address.
   *
   * @note SocketAddress::tryConvertToIPv4 is no-throw variant of this function
   */
  void convertToIPv4();

  /**
   * Try to convert an address to IPv4.
   *
   * This attempts to convert an address to an IPv4 address if possible.
   * If the address is an IPv4-mapped IPv6 address, it is converted to an IPv4
   * address and true is returned.  Otherwise nothing is done, and false is
   * returned.
   *
   * @return true if the address was converted to IPv4-mapped address, false
   * otherwise
   */
  bool tryConvertToIPv4();

  /**
   * Convert an IPv4 address to IPv6 [::ffff:a.b.c.d]
   *
   * @return true if the address conversion was done, false otherwise
   */
  bool mapToIPv6();

  /**
   * Get string representation of the host name (or IP address if the host name
   * cannot be resolved).
   *
   * Warning: Using this method is strongly discouraged.  It performs a
   * DNS lookup, which may block for many seconds.
   *
   * Raises std::invalid_argument if an error occurs.
   *
   * @return Host name (or IP address)
   */
  std::string getHostStr() const;

  /**
   * Get the path name for a Unix domain socket.
   *
   * Returns a std::string containing the path.  For anonymous sockets, an
   * empty string is returned.
   *
   * For addresses in the abstract namespace (Linux-specific), a std::string
   * containing binary data is returned.  In this case the first character will
   * always be a NUL character.
   *
   * Raises std::invalid_argument if called on a non-Unix domain socket.
   *
   * @return Path name for a Unix domain socket
   */
  std::string getPath() const;

#if FOLLY_HAVE_VSOCK
  /**
   * Get the CID (Context Identifier) for a VSOCK address.
   *
   * Raises std::invalid_argument if called on a non-VSOCK address.
   *
   * @return CID for a VSOCK address
   */
  uint32_t getVsockCID() const;
#endif

  /**
   * Get human-readable string representation of the address.
   *
   * This prints a string representation of the address, for human consumption.
   * For IP addresses, the string is of the form "<IP>:<port>".
   *
   * @return Human-readable representation of the address
   */
  std::string describe() const;

  bool operator==(const SocketAddress& other) const;

  /**
   * Check whether the first N bits of this address match the first N
   * bits of another address.
   *
   * @note returns false if the addresses are not from the same
   *       address family or if the family is neither IPv4 nor IPv6
   *
   * @param other The address to match against
   * @param prefixLength Length of the prefix to match
   * @return true if `prefixLength` this address matches with `other`,
   * false otherwise
   */
  bool prefixMatch(const SocketAddress& other, unsigned prefixLength) const;

  /**
   * Use this operator for storing maps based on SocketAddress.
   */
  bool operator<(const SocketAddress& other) const;

  /**
   * Compuate a hash of a SocketAddress.
   *
   * @return Hash for this SocketAddress
   */
  size_t hash() const;

 private:
  /**
   * Unix socket addresses require more storage than IPv4 and IPv6 addresses,
   * and are comparatively little-used.
   *
   * Therefore SocketAddress' internal storage_ member variable doesn't
   * contain room for a full unix address, to avoid wasting space in the common
   * case.  When we do need to store a Unix socket address, we use this
   * ExternalUnixAddr structure to allocate a struct sockaddr_un separately on
   * the heap.
   */
  struct ExternalUnixAddr {
    struct sockaddr_un* addr;
    socklen_t len;

    socklen_t pathLength() const {
      return socklen_t(len - offsetof(struct sockaddr_un, sun_path));
    }

    ExternalUnixAddr() {
      addr = new struct sockaddr_un;
      addr->sun_family = AF_UNIX;
      len = 0;
    }

    ExternalUnixAddr(const ExternalUnixAddr& other) : ExternalUnixAddr() {
      len = other.len;
      memcpy(addr, other.addr, size_t(len));
    }

    ExternalUnixAddr& operator=(const ExternalUnixAddr& other) {
      if (this != &other) {
        len = other.len;
        memcpy(addr, other.addr, size_t(len));
      }
      return *this;
    }

    ~ExternalUnixAddr() { delete addr; }
  };

  /**
   * This class stores an IP address and port.
   */
  struct IPAddr {
    folly::IPAddress ip;
    uint16_t port;

    IPAddr() : ip(), port(0) {}
    IPAddr(const folly::IPAddress& ip_, uint16_t port_)
        : ip(ip_), port(port_) {}
  };

  /**
   * This class stores the CID (Context Identifier) and port for VSOCK
   * addresses.
   */
  struct VsockAddr {
    uint32_t cid;
    uint32_t port;

    explicit VsockAddr(uint32_t cid_) : cid(cid_), port(0) {}
    VsockAddr(uint32_t cid_, uint32_t port_) : cid(cid_), port(port_) {}

#if FOLLY_HAVE_VSOCK
    const char* getMappedName() const;
#endif
  };

  bool holdsInet() const { return std::holds_alternative<IPAddr>(storage_); }

  bool holdsUnix() const {
    return std::holds_alternative<ExternalUnixAddr>(storage_);
  }

  bool holdsVsock() const {
    return std::holds_alternative<VsockAddr>(storage_);
  }

  struct addrinfo* getAddrInfo(const char* host, uint16_t port, int flags);
  struct addrinfo* getAddrInfo(const char* host, const char* port, int flags);
  void setFromAddrInfo(const struct addrinfo* info);
  void setFromLocalAddr(const struct addrinfo* info);
  void setFromSocket(
      NetworkSocket socket,
      int (*fn)(NetworkSocket, struct sockaddr*, socklen_t*));
  std::string getIpString(int flags) const;
  void getIpString(char* buf, size_t buflen, int flags) const;

  void updateUnixAddressLength(socklen_t addrlen);

  /*
   * storage_ contains either an IPAddr, an ExternalUnixAddr, or a VsockAddr.
   * IPAddr is used for IPv4 and IPv6 addresses.
   * ExternalUnixAddr is used for Unix domain sockets.
   * VsockAddr is used for VSOCK addresses.
   */
  std::variant<IPAddr, ExternalUnixAddr, VsockAddr> storage_{IPAddr()};
};

/**
 * Hash a SocketAddress object.
 *
 * boost::hash uses hash_value(), so this allows boost::hash to automatically
 * work for SocketAddress.
 */
size_t hash_value(const SocketAddress& address);

std::ostream& operator<<(std::ostream& os, const SocketAddress& addr);
} // namespace folly

namespace std {

// Provide an implementation for std::hash<SocketAddress>
template <>
struct hash<folly::SocketAddress> {
  size_t operator()(const folly::SocketAddress& addr) const {
    return addr.hash();
  }
};
} // namespace std
