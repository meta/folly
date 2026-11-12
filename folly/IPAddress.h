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

/**
 * Provides a unified interface for IP addresses.
 *
 * @refcode folly/docs/examples/folly/ipaddress.cpp
 *
 * @class folly::IPAddress
 * @see IPAddressV6
 * @see IPAddressV4
 */

#pragma once

#include <compare>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <type_traits>
#include <utility> // std::pair

#include <folly/ConstexprMath.h>
#include <folly/IPAddressException.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/Range.h>
#include <folly/detail/IPAddress.h>
#include <folly/lang/Exception.h>

namespace folly {

class IPAddress;

/**
 * Pair of IPAddress, netmask
 */
using CIDRNetwork = std::pair<IPAddress, uint8_t>;

class IPAddress {
 private:
  template <typename F>
  auto pick(F f) const {
    return isV4() ? f(asV4()) : isV6() ? f(asV6()) : f(asNone());
  }

  class IPAddressNone {
   public:
    bool isZero() const { return true; }
    size_t bitCount() const { return 0; }
    std::string toJson() const {
      return "{family:'AF_UNSPEC', addr:'', hash:0}";
    }
    std::size_t hash() const { return std::hash<uint64_t>{}(0); }
    bool isLoopback() const {
      throw_exception<InvalidAddressFamilyException>("empty address");
    }
    bool isLinkLocal() const {
      throw_exception<InvalidAddressFamilyException>("empty address");
    }
    bool isLinkLocalBroadcast() const {
      throw_exception<InvalidAddressFamilyException>("empty address");
    }
    bool isNonroutable() const {
      throw_exception<InvalidAddressFamilyException>("empty address");
    }
    bool isPrivate() const {
      throw_exception<InvalidAddressFamilyException>("empty address");
    }
    bool isMulticast() const {
      throw_exception<InvalidAddressFamilyException>("empty address");
    }
    IPAddress mask(uint8_t numBits) const {
      (void)numBits;
      return IPAddress();
    }
    std::string str() const { return ""; }
    std::string toFullyQualified() const { return ""; }
    void toFullyQualifiedAppend(std::string& out) const {
      (void)out;
      return;
    }
    uint8_t version() const { return 0; }
    const unsigned char* bytes() const { return nullptr; }
  };

  IPAddressNone const& asNone() const {
    if (!empty()) {
      throw_exception<InvalidAddressFamilyException>("not empty");
    }
    return addr_.ipNoneAddr;
  }

 public:
  /**
   * Returns true if the input string can be parsed as an IP address.
   */
  static bool validate(StringPiece ip) noexcept;
  /**
   * Return the IPAddressV4 representation of the address, converting
   * from V6 to V4 if needed.
   *
   *
   *
   * @throws IPAddressFormatException if the V6 Address is not IPv4 mappes
   */
  static IPAddressV4 createIPv4(const IPAddress& addr);

  /**
   * Return the address as a IPAddressV6, converting from V4 to V6 if
   * needed.
   */
  static IPAddressV6 createIPv6(const IPAddress& addr);

  /**
   * Create a network and mask from a CIDR formatted address string.
   *
   * @param [in] ipSlashCidr IP/CIDR formatted string to split
   * @param [in] defaultCidr default value if no /N specified (if defaultCidr
   *             is -1, will use /32 for IPv4 and /128 for IPv6)
   * @param [in] mask apply mask on the address or not,
   *             e.g. 192.168.13.46/24 => 192.168.13.0/24
   * @return either pair with IPAddress network and uint8_t mask or
   *         CIDRNetworkError
   */
  static Expected<CIDRNetwork, CIDRNetworkError> tryCreateNetwork(
      StringPiece ipSlashCidr, int defaultCidr = -1, bool mask = true);

  /**
   * Create a network and mask from a CIDR formatted address string.
   *
   * Same as tryCreateNetwork() but throws on error.
   *
   * @throws IPAddressFormatException if `ipSlashCidr` is not a valid
   *         CIDR-formatted address
   * @return pair with IPAddress network and uint8_t mask
   */
  static CIDRNetwork createNetwork(
      StringPiece ipSlashCidr, int defaultCidr = -1, bool mask = true);

  /**
   * Return a string representation of a CIDR block created with
   * createNetwork().
   *
   * @param [in] network pair of address and cidr
   * @return string representing the netblock
   */
  static std::string networkToString(const CIDRNetwork& network);

  /**
   * Create a new IPAddress instance from the provided binary data
   * in network byte order.
   *
   * @throws IPAddressFormatException if the length of `bytes` is not 4 or 16.
   *
   */
  static IPAddress fromBinary(ByteRange bytes);

  /**
   * Non-throwing version of fromBinary().
   * On failure returns IPAddressFormatError.
   */
  static Expected<IPAddress, IPAddressFormatError> tryFromBinary(
      ByteRange bytes) noexcept;

  /**
   * Tries to create a new IPAddress instance from provided string.
   *
   * On failure, returns IPAddressFormatError.
   */
  static Expected<IPAddress, IPAddressFormatError> tryFromString(
      StringPiece str) noexcept;

  /**
   * Tries to create a new IPAddress instance from the provided sockaddr.
   *
   * On failure, returns IPAddressFormatError.
   */
  static Expected<IPAddress, IPAddressFormatError> tryFromSockAddr(
      const sockaddr* addr) noexcept;

  /**
   * Create an IPAddress from a `uint32_t`, using network byte order.
   *
   * @throws IPAddressFormatException if `src` does not represent a valid IP
   * Address.
   */
  static IPAddress fromLong(uint32_t src);

  /**
   * Create an IPAddress from a `uint32_t`, using host byte order.
   *
   * @throws IPAddressFormatException if `src` does not represent a valid IP
   * address.
   */
  static IPAddress fromLongHBO(uint32_t src);

  /**
   * Given 2 (IPAddress, mask) pairs, extract the longest common pair.
   */
  static CIDRNetwork longestCommonPrefix(
      const CIDRNetwork& one, const CIDRNetwork& two);

  /**
   * Constructs an uninitialized IPAddress.
   */
  IPAddress();

  /**
   * Parse an IPAddress from a string representation.
   *
   * Formats accepted are exactly the same as the ones accepted by
   * `inet_pton()`, using `AF_INET6` if the string contains colons, and `AF_INET
   * `otherwise; with the exception that the whole address can optionally be
   * enclosed in square brackets.
   *
   * @throws IPAddressFormatException if `str` is not a valid IP Address.
   */
  explicit IPAddress(StringPiece str);

  /**
   * Create an IPAddress from a `sockaddr` struct.
   *
   * @throws IPAddressFormatException if `addr` is a nullptr, or not `AF_INET`
   * or `AF_INET6`
   */
  explicit IPAddress(const sockaddr* addr);

  /**
   * Create an IPAddress from an IPAddressV4
   */
  /* implicit */ IPAddress(const IPAddressV4 ipV4Addr) noexcept;

  /**
   * Create an IPAddress from an `in_addr` representation of an IPV4 address
   */
  /* implicit */ IPAddress(const in_addr addr) noexcept;

  /**
   * Create an IPAddress from an IPAddressV6
   */
  /* implicit */ IPAddress(const IPAddressV6& ipV6Addr) noexcept;

  /**
   * Create an IPAddress from an `in6_addr` representation of an IPV6 address
   */
  /* implicit */ IPAddress(const in6_addr& addr) noexcept;

  /**
   * @overloadbrief Copy assignment from other IPAddress representations
   *
   * Copy assignment from an IPAddressV4
   */
  IPAddress& operator=(const IPAddressV4& ipv4_addr) noexcept;

  /**
   * Copy assignment from an IPAddressV6
   */
  IPAddress& operator=(const IPAddressV6& ipv6_addr) noexcept;

  /**
   * Converts an IPAddress to an IPAddressV4 instance.
   *
   * @note This is not some handy convenience wrapper to convert an IPv4 address
   *       to a mapped IPv6 address. If you want that use createIPv6()
   *
   * @throws InvalidAddressFamilyException if the IP Address is not currently
   * a valid V4 instance.
   */
  const IPAddressV4& asV4() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    if (!isV4()) [[unlikely]] {
      asV4Throw();
    }
    return addr_.ipV4Addr;
  }

  /**
   * Converts an IPAddress to an IPAddressV6 instance
   *
   * @throws InvalidAddressFamilyException if the IP Address is not currently
   * a valid V6 instance.
   */
  const IPAddressV6& asV6() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    if (!isV6()) [[unlikely]] {
      asV6Throw();
    }
    return addr_.ipV6Addr;
  }

  /**
   * Return then `sa_family_t` of the IP Address
   */
  sa_family_t family() const { return family_; }

  /**
   * Given a `sockaddr_storage` struct, populate it with an appropriate value.
   *
   * @param [out] dest The struct to populate
   * @param port The port number to put in the struct (defaults to 0)
   *
   */
  int toSockaddrStorage(sockaddr_storage* dest, uint16_t port = 0) const {
    if (dest == nullptr) {
      throw_exception<IPAddressFormatException>("dest must not be null");
    }
    memset(dest, 0, sizeof(sockaddr_storage));
    dest->ss_family = family();

    if (isV4()) {
      sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(dest);
      sin->sin_addr = asV4().toAddr();
      sin->sin_port = port;
#if defined(__APPLE__)
      sin->sin_len = sizeof(*sin);
#endif
      return sizeof(*sin);
    } else if (isV6()) {
      sockaddr_in6* sin = reinterpret_cast<sockaddr_in6*>(dest);
      sin->sin6_addr = asV6().toAddr();
      sin->sin6_port = port;
      sin->sin6_scope_id = asV6().getScopeId();
#if defined(__APPLE__)
      sin->sin6_len = sizeof(*sin);
#endif
      return sizeof(*sin);
    } else {
      throw_exception<InvalidAddressFamilyException>(family());
    }
  }

  /**
   * @overloadbrief Check if the address is found in the specified CIDR
   * netblock.
   *
   * This will return false if the specified cidrNet is V4, but the address is
   * V6. It will also return false if the specified cidrNet is V6 but the
   * address is V4. This method will do the right thing in the case of a v6
   * mapped v4 address.
   *
   * @note This is slower than the below counterparts. If perf is important use
   *       one of the two argument variations below.
   * @param [in] cidrNetwork address in "192.168.1.0/24" format
   * @throws IPAddressFormatException if no /mask in cidrNetwork
   * @return true if address is part of specified subnet with cidr
   */
  bool inSubnet(StringPiece cidrNetwork) const;

  /**
   * Check if an IPAddress belongs to a subnet.
   * @param [in] subnet Subnet to check against (e.g. 192.168.1.0)
   * @param [in] cidr   CIDR for subnet (e.g. 24 for /24)
   * @return true if address is part of specified subnet with cidr
   */
  bool inSubnet(const IPAddress& subnet, uint8_t cidr) const;

  /**
   * Check if an IPAddress belongs to the subnet with the given mask.
   *
   * This is the same as inSubnet but the mask is provided instead of looked up
   * from the cidr.
   * @param [in] subnet Subnet to check against
   * @param [in] mask   The netmask for the subnet
   * @return true if address is part of the specified subnet with mask
   */
  bool inSubnetWithMask(const IPAddress& subnet, ByteRange mask) const;

  /**
   * Returns true if address is a v4 mapped address
   */
  bool isIPv4Mapped() const { return isV6() && asV6().isIPv4Mapped(); }

  /**
   * Returns true if address is uninitialised
   */
  bool empty() const { return family_ == AF_UNSPEC; }

  /**
   * Returns true if address is initalised
   */
  explicit operator bool() const { return !empty(); }

  /**
   * Returns true if this represents an IPv4 address
   */
  bool isV4() const { return family_ == AF_INET; }

  /**
   * Returns true if this represents an IPv6 address
   */
  bool isV6() const { return family_ == AF_INET6; }

  /**
   * Returns true if the address is all zeros
   */
  bool isZero() const {
    return pick([&](auto& _) { return _.isZero(); });
  }

  /**
   * The number of bits in the address representation
   */
  size_t bitCount() const {
    return pick([&](auto& _) { return _.bitCount(); });
  }

  /**
   * The number of bytes in the address representation
   */
  size_t byteCount() const { return bitCount() / 8; }

  /**
   * Get the nth most significant bit of the IP address (0-indexed).
   * @param bitIndex n
   */
  bool getNthMSBit(size_t bitIndex) const {
    return detail::getNthMSBitImpl(*this, bitIndex, family());
  }

  /**
   * Get the nth most significant byte of the IP address (0-indexed).
   * @param byteIndex n
   */
  uint8_t getNthMSByte(size_t byteIndex) const;

  /**
   * Get the nth bit of the IP address (0-indexed).
   * @param bitIndex n
   */
  bool getNthLSBit(size_t bitIndex) const {
    return getNthMSBit(bitCount() - bitIndex - 1);
  }

  /**
   * Get the nth byte of the IP address (0-indexed).
   * @param byteIndex n
   */
  uint8_t getNthLSByte(size_t byteIndex) const {
    return getNthMSByte(byteCount() - byteIndex - 1);
  }

  /**
   * Get a json representation of the IP address.
   *
   * This prints a string representation of the address, for human consumption
   * or logging. The string will take the form of a JSON object that looks like:
   *  `{family:'AF_INET|AF_INET6', addr:'address', hash:long}`.
   */
  std::string toJson() const {
    return pick([&](auto& _) { return _.toJson(); });
  }

  /**
   * Returns a hash of the IP address.
   */
  std::size_t hash() const {
    return pick([&](auto& _) { return _.hash(); });
  }

  /**
   * Return true if the IP address qualifies as localhost.
   */
  bool isLoopback() const {
    return pick([&](auto& _) { return _.isLoopback(); });
  }

  /**
   * Return true if the IP address qualifies as link local
   */
  bool isLinkLocal() const {
    return pick([&](auto& _) { return _.isLinkLocal(); });
  }

  /**
   * Return true if the IP address qualifies as broadcast.
   */
  bool isLinkLocalBroadcast() const {
    return pick([&](auto& _) { return _.isLinkLocalBroadcast(); });
  }

  /**
   * Return true if the IP address is a special purpose address, as defined per
   * RFC 6890 (i.e. 0.0.0.0).
   *
   * For V6, true if the address is not in one of global scope blocks:
   * 2000::/3, ffxe::/16.
   */
  bool isNonroutable() const {
    return pick([&](auto& _) { return _.isNonroutable(); });
  }

  /**
   * Return true if the IP address is private, as per RFC 1918 and RFC 4193.
   *
   * For example, 192.168.xxx.xxx or fc00::/7 addresses.
   */
  bool isPrivate() const {
    return pick([&](auto& _) { return _.isPrivate(); });
  }

  /**
   * Return true if the IP address is a multicast address.
   */
  bool isMulticast() const {
    return pick([&](auto& _) { return _.isMulticast(); });
  }

  /**
   * Creates an IPAddress instance with all but most significant numBits set to
   * 0.
   *
   * @throws IPAddressFormatException if numBits > bitCount()
   *
   * @param [in] numBits number of bits to mask
   * @return IPAddress instance with bits set to 0
   */
  IPAddress mask(uint8_t numBits) const {
    return pick([&](auto& _) { return IPAddress(_.mask(numBits)); });
  }

  /**
   * Provides a string representation of address.
   *
   * @throws IPAddressFormatException on `inet_ntop` error.
   *
   * @note The string representation is calculated on demand.
   */
  std::string str() const {
    return pick([&](auto& _) { return _.str(); });
  }

  /**
   * Return the fully qualified string representation of the address.
   *
   * For V4 addresses this is the same as calling str(). For V6 addresses
   * this is the hex representation with : characters inserted every 4 digits.
   */
  std::string toFullyQualified() const {
    return pick([&](auto& _) { return _.toFullyQualified(); });
  }

  /**
   * Same as toFullyQualified() but append to an output string.
   */
  void toFullyQualifiedAppend(std::string& out) const {
    return pick([&](auto& _) { return _.toFullyQualifiedAppend(out); });
  }

  /**
   * Returns the IP address version. 0 if empty, 4 or 6 if nonempty.
   */
  uint8_t version() const {
    return pick([&](auto& _) { return _.version(); });
  }

  /**
   * Returns a pointer to the to IP address bytes, in network byte order.
   */
  const unsigned char* bytes() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return pick([&](auto& _) { return _.bytes(); });
  }

 private:
  [[noreturn]] void asV4Throw() const;
  [[noreturn]] void asV6Throw() const;

  union IPAddressV46 {
    IPAddressNone ipNoneAddr;
    IPAddressV4 ipV4Addr;
    IPAddressV6 ipV6Addr;
    IPAddressV46() noexcept : ipNoneAddr() {}
    explicit IPAddressV46(const IPAddressV4& addr) noexcept : ipV4Addr(addr) {}
    explicit IPAddressV46(const IPAddressV6& addr) noexcept : ipV6Addr(addr) {}
  };
  IPAddressV46 addr_;
  sa_family_t family_;
};

/**
 * `boost::hash` uses hash_value(), so this allows `boost::hash` to work
 * automatically for IPAddress
 */
std::size_t hash_value(const IPAddress& addr);

/**
 * Appends a string representation of the IP address to the stream using str().
 */
std::ostream& operator<<(std::ostream& os, const IPAddress& addr);

/**
 * @overloadbrief Define toAppend() to allow IPAddress to be used with
 * `folly::to<string>`
 */
void toAppend(IPAddress addr, std::string* result);
void toAppend(IPAddress addr, fbstring* result);

/**
 * Return true if two addresses are equal.
 *
 * V4-to-V6-mapped addresses are compared as V4 addresses.
 *
 * @return true if the two addresses are equal.
 */
bool operator==(const IPAddress& addr1, const IPAddress& addr2);

/**
 * Three-way comparison for IPAddress.
 *
 * V4-to-V6-mapped addresses are compared as V4 addresses.
 */
std::strong_ordering operator<=>(
    const IPAddress& addr1, const IPAddress& addr2) noexcept;

} // namespace folly

namespace std {
template <>
struct hash<folly::IPAddress> {
  size_t operator()(const folly::IPAddress& addr) const { return addr.hash(); }
};
} // namespace std
