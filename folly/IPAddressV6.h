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
 * A representation of an IPv6 address
 *
 * @see IPAddress
 * @see IPAddressV4
 *
 * @class folly::IPAddressV6
 */

#pragma once

#include <cstring>

#include <array>
#include <compare>
#include <functional>
#include <iosfwd>
#include <map>
#include <stdexcept>

#include <folly/Expected.h>
#include <folly/FBString.h>
#include <folly/IPAddressException.h>
#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/detail/IPAddress.h>
#include <folly/hash/Hash.h>

namespace folly {

class IPAddress;
class IPAddressV4;
class IPAddressV6;
class MacAddress;

/**
 * Pair of IPAddressV6, netmask
 */
using CIDRNetworkV6 = std::pair<IPAddressV6, uint8_t>;

/**
 * Specialization for `std::array` for IPv6 addresses
 */
using ByteArray16 = std::array<uint8_t, 16>;

class IPAddressV6 {
 public:
  /**
   * Represents the different types that IPv6 Addresses can be
   *
   */
  enum Type {
    TEREDO,
    T6TO4,
    NORMAL,
  };

  /**
   * A constructor parameter to indicate that we should create a link-local
   * IPAddressV6.
   */
  enum LinkLocalTag {
    LINK_LOCAL,
  };

  /**
   * Alias std::runtime_error, to be thrown when a type assertion fails
   */
  using TypeError = std::runtime_error;

  /**
   * The binary prefix for Teredo networks
   */
  static const uint32_t PREFIX_TEREDO;

  /**
   * The binary prefix for Teredo networks
   */
  static const uint32_t PREFIX_6TO4;

  /**
   * The size of the std::string returned by toFullyQualified.
   */
  static constexpr size_t kToFullyQualifiedSize =
      8 /*words*/ * 4 /*hex chars per word*/ + 7 /*separators*/;

  /**
   * Return true if the input string can be parsed as an IPv6 addres
   */
  static bool validate(StringPiece ip) noexcept;

  /**
   * Create a new IPAddressV6 instance from the provided binary data, in network
   * byte order.
   *
   * @throws IPAddressFormatException if the input length is not 16 bytes.
   */
  static IPAddressV6 fromBinary(ByteRange bytes);

  /**
   * Create a new IPAddressV6 from the provided ByteRange.
   *
   * Returns an IPAddressFormatError if the input length is not 4 bytes.
   */
  static Expected<IPAddressV6, IPAddressFormatError> tryFromBinary(
      ByteRange bytes) noexcept;

  /**
   * Create a new IPAddressV6 from the provided string.
   *
   * Returns an IPAddressFormatError if the string is not a valid IP.
   */
  static Expected<IPAddressV6, IPAddressFormatError> tryFromString(
      StringPiece str) noexcept;

  /**
   * Create a new IPAddress instance from the ip6.arpa representation.
   * @throws IPAddressFormatException if the input is not a valid ip6.arpa
   * representation
   */
  static IPAddressV6 fromInverseArpaName(const std::string& arpaname);

  /**
   * Returns the address as a ByteRange.
   */
  ByteRange toBinary() const {
    return ByteRange((const unsigned char*)&addr_.in6Addr_.s6_addr, 16);
  }

  /**
   * Default constructor for IPAddressV6.
   *
   * The address value will be ::0
   */
  IPAddressV6();

  /**
   * Construct an IPAddressV6 from a string
   *
   * @throws IPAddressFormatException if the string is not a valid IPv6 address.
   */
  explicit IPAddressV6(StringPiece addr);

  /**
   * Construct an IPAddressV6 from a ByteArray16
   */
  explicit IPAddressV6(const ByteArray16& src) noexcept;

  /**
   * Construct an IPAddressV6 from an `in_addr` representation of an IPV6
   * address
   */
  explicit IPAddressV6(const in6_addr& src) noexcept;

  /**
   * Construct an IPAddressV6 from an `sockaddr_in6` representation of an IPV6
   * address
   */
  explicit IPAddressV6(const sockaddr_in6& src) noexcept;

  /**
   * Create a link-local IPAddressV6 from the specified ethernet MAC address.
   */
  IPAddressV6(LinkLocalTag tag, MacAddress mac);

  /**
   * Return the mapped IPAddressV4
   *
   * @throws IPAddressFormatException if the address is not IPv4 mapped
   */
  IPAddressV4 createIPv4() const;

  /**
   * Return a V4 address if this is a 6To4 address.
   * @throws TypeError if not a 6To4 address
   */
  IPAddressV4 getIPv4For6To4() const;

  /**
   * Return true if the address is a 6to4 address
   */
  bool is6To4() const { return type() == IPAddressV6::Type::T6TO4; }

  /**
   * Return true if the address is a Teredo address
   */
  bool isTeredo() const { return type() == IPAddressV6::Type::TEREDO; }

  /**
   * Return true if the adddress is IPv4 mapped
   */
  bool isIPv4Mapped() const;

  /**
   * Return what type of IPv6 address this is.
   *
   * @see Type
   */
  Type type() const;

  /**
   * Return the number of bits in the IP address representation
   *
   * @returns 128
   */
  static constexpr size_t bitCount() { return 128; }

  /**
   * Get a json representation of the IP address.
   *
   * This prints a string representation of the address, for human consumption
   * or logging. The string will take the form of a JSON object that looks like:
   *  `{family:'AF_INET6', addr:'address', hash:long}`.
   */
  std::string toJson() const;

  /**
   * Returns a hash of the IP address.
   */
  size_t hash() const;

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
   *
   * @param [in] subnet Subnet to check against (e.g. 192.168.1.0)
   * @param [in] cidr   CIDR for subnet (e.g. 24 for /24)
   * @return true if address is part of specified subnet with cidr
   */
  bool inSubnet(const IPAddressV6& subnet, uint8_t cidr) const {
    return inSubnetWithMask(subnet, fetchMask(cidr));
  }

  /**
   * Check if an IPAddress belongs to the subnet with the given mask.
   *
   * This is the same as inSubnet but the mask is provided instead of looked up
   * from the cidr.
   * @param [in] subnet Subnet to check against
   * @param [in] mask   The netmask for the subnet
   * @return true if address is part of the specified subnet with mask
   */
  bool inSubnetWithMask(
      const IPAddressV6& subnet, const ByteArray16& mask) const;

  /**
   * Return true if the IP address qualifies as localhost.
   */
  bool isLoopback() const;

  /**
   * Return true if the IP address is a special purpose address, as defined per
   * RFC 6890.
   *
   */
  bool isNonroutable() const { return !isRoutable(); }

  /**
   * Return true if this address is routable.
   */
  bool isRoutable() const;

  /**
   * Return true if the IP address is private, as per RFC 1918 and RFC 4193.
   *
   * For example, 192.168.xxx.xxx or fc00::/7 addresses.
   */
  bool isPrivate() const;

  /**
   * Return true if this is a link-local IPv6 address.
   *
   * Note that this only returns true for addresses in the fe80::/10 range.
   * It returns false for the loopback address (::1), even though this address
   * is also effectively has link-local scope.  It also returns false for
   * link-scope and interface-scope multicast addresses.
   */
  bool isLinkLocal() const;

  /**
   * Return the mac address if this is a link-local IPv6 address.
   *
   * @return a `folly::Optional<MacAddress>` union representing the mac address.
   *
   * If the address is not a link-local one it will return an empty Optional.
   * You can use Optional::value() to check whether the mac address is not null.
   */
  Optional<MacAddress> getMacAddressFromLinkLocal() const;

  /**
   * Return the mac address if this is an auto-configured IPv6 address based on
   * EUI-64
   *
   * If the address is not based on EUI-64 it will return an empty
   * Optional. You can use Optional::value() to check whether the mac address is
   * not null.
   *
   * @return a `folly::Optional<MacAddress>` union representing the mac address.
   *
   */
  Optional<MacAddress> getMacAddressFromEUI64() const;

  /**
   * Return true if this is a multicast address.
   */
  bool isMulticast() const;

  /**
   * Return the flags for a multicast address.
   *
   * This method may only be called on multicast addresses.
   */
  uint8_t getMulticastFlags() const;

  /**
   * Return the scope for a multicast address.
   *
   * This method may only be called on multicast addresses.
   */
  uint8_t getMulticastScope() const;

  /**
   * Return true if the address is 0
   */
  bool isZero() const {
    constexpr auto zero = ByteArray16{{}};
    return 0 == std::memcmp(bytes(), zero.data(), zero.size());
  }

  /**
   * Return true if the IP address qualifies as broadcast.
   */
  bool isLinkLocalBroadcast() const;

  /**
   * Creates an IPAddressV6 instance with all but most significant numBits set
   * to 0.
   *
   * @throws IPAddressFormatException if `numBits > bitCount()`
   *
   * @param [in] numBits number of bits to mask
   * @return IPAddress instance with bits set to 0
   */
  IPAddressV6 mask(size_t numBits) const;

  /**
   * Return the underlying `in6_addr` structure
   */
  in6_addr toAddr() const { return addr_.in6Addr_; }

  /**
   * Return the link-local scope id.
   *
   * This should always be 0 for IP addresses that are *not* link-local.
   *
   */
  uint16_t getScopeId() const { return scope_; }
  /**
   * Set the link-local scope id.
   *
   * This should always be 0 for IP addresses that are *not* link-local.
   *
   */
  void setScopeId(uint16_t scope) { scope_ = scope; }

  /**
   * Return the IP address represented as a `sockaddr_in6` struct
   *
   */
  sockaddr_in6 toSockAddr() const {
    sockaddr_in6 addr;
    memset(&addr, 0, sizeof(sockaddr_in6));
    addr.sin6_family = AF_INET6;
    addr.sin6_scope_id = scope_;
    memcpy(&addr.sin6_addr, &addr_.in6Addr_, sizeof(in6_addr));
    return addr;
  }

  /**
   * Return a ByteArray16 containing the bytes of the IP address.
   */
  ByteArray16 toByteArray() const {
    ByteArray16 ba{{0}};
    std::memcpy(ba.data(), bytes(), 16);
    return ba;
  }

  /**
   * Return the fully qualified string representation of the address.
   *
   * This is the hex representation with : characters inserted every 4 digits.
   */
  std::string toFullyQualified() const;

  /**
   * Same as toFullyQualified() but append to an output string.
   */
  void toFullyQualifiedAppend(std::string& out) const;

  /**
   * Create the inverse arpa representation of the IP address.
   *
   */
  std::string toInverseArpaName() const;

  /**
   * Provides a string representation of address.
   *
   * Throws an IPAddressFormatException on `inet_ntop` error.
   *
   * The string representation is calculated on demand.
   */
  std::string str() const;

  /**
   * Returns the version of the IP Address.
   *
   * @returns 6
   */
  uint8_t version() const { return 6; }

  /**
   * Return the solicited-node multicast address for this address.
   */
  IPAddressV6 getSolicitedNodeAddress() const;

  /**
   * Return the mask associated with the given number of bits.
   *
   * If for instance numBits was 24 (e.g. /24) then the V4 mask returned should
   * be {0xff, 0xff, 0xff, 0x00}.
   * @param [in] numBits bitmask to retrieve
   * @throws abort if numBits == 0 or numBits > bitCount()
   * @return mask associated with numBits
   */

  static ByteArray16 fetchMask(size_t numBits);

  /**
   * Given 2 (IPAddressV6, mask) pairs extract the longest common (IPAddressV6,
   * mask) pair
   */
  static CIDRNetworkV6 longestCommonPrefix(
      const CIDRNetworkV6& one, const CIDRNetworkV6& two);

  /**
   * The number of bytes in the IP address
   *
   * @returns 16
   */
  static constexpr size_t byteCount() { return 16; }

  /**
   * Get the nth most significant bit of the IP address (0-indexed).
   * @param bitIndex n
   */
  bool getNthMSBit(size_t bitIndex) const {
    return detail::getNthMSBitImpl(*this, bitIndex, AF_INET6);
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
   * Returns a pointer to the to IP address bytes, in network byte order.
   */
  const unsigned char* bytes() const { return addr_.in6Addr_.s6_addr; }

 protected:
  /**
   * Helper that returns true if the address is in the binary subnet specified
   * by addr.
   */
  bool inBinarySubnet(const std::array<uint8_t, 2> addr, size_t numBits) const;

 private:
  auto tie() const { return std::tie(addr_.bytes_, scope_); }

 public:
  /**
   * Return true if the two addresses are equal.
   */
  friend inline bool operator==(
      const IPAddressV6& addr1, const IPAddressV6& addr2) {
    return addr1.tie() == addr2.tie();
  }

  /**
   * Three-way comparison for IPAddressV6.
   */
  friend auto operator<=>(const IPAddressV6& a, const IPAddressV6& b) noexcept {
    return a.tie() <=> b.tie();
  }

 private:
  union AddressStorage {
    in6_addr in6Addr_;
    ByteArray16 bytes_;
    AddressStorage() { std::memset(this, 0, sizeof(AddressStorage)); }
    explicit AddressStorage(const ByteArray16& bytes) : bytes_(bytes) {}
    explicit AddressStorage(const in6_addr& addr) : in6Addr_(addr) {}
    explicit AddressStorage(MacAddress mac);
  } addr_;

  // Link-local scope id.  This should always be 0 for IPAddresses that
  // are *not* link-local.
  uint16_t scope_{0};

  /**
   * Set the current IPAddressV6 object to have the address specified by bytes.
   * Returns IPAddressFormatError if bytes.size() is not 16.
   */
  Expected<Unit, IPAddressFormatError> trySetFromBinary(
      ByteRange bytes) noexcept;
};

/**
 * `boost::hash` uses hash_value(), so this allows `boost::hash` to work
 * automatically for IPAddressV4
 */
std::size_t hash_value(const IPAddressV6& addr);

/**
 * Appends a string representation of the IP address to the stream using str().
 */

std::ostream& operator<<(std::ostream& os, const IPAddressV6& addr);

/**
 * @overloadbrief Define toAppend() to allow IPAddress to be used with
 * `folly::to<string>`
 */
void toAppend(IPAddressV6 addr, std::string* result);
void toAppend(IPAddressV6 addr, fbstring* result);

} // namespace folly

namespace std {
template <>
struct hash<folly::IPAddressV6> {
  size_t operator()(const folly::IPAddressV6& addr) const {
    return addr.hash();
  }
};
} // namespace std
