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
 * A representation of an IPv4 address
 *
 * @class folly::IPAddressV4
 * @see IPAddress
 * @see IPAddressV6
 */

#pragma once

#include <cstring>

#include <array>
#include <compare>
#include <functional>
#include <iosfwd>

#include <folly/Expected.h>
#include <folly/FBString.h>
#include <folly/IPAddressException.h>
#include <folly/Range.h>
#include <folly/detail/IPAddress.h>
#include <folly/hash/Hash.h>

namespace folly {

class IPAddress;
class IPAddressV4;
class IPAddressV6;

/**
 * Pair of IPAddressV4, netmask
 */
using CIDRNetworkV4 = std::pair<IPAddressV4, uint8_t>;

/**
 * Specialization of `std::array` for IPv4 addresses
 */
using ByteArray4 = std::array<uint8_t, 4>;

class IPAddressV4 {
 public:
  /**
   * Max size of std::string returned by toFullyQualified()
   */
  static constexpr size_t kMaxToFullyQualifiedSize =
      4 /*words*/ * 3 /*max chars per word*/ + 3 /*separators*/;

  /**
   * Returns true if the input string can be parsed as an IP address.
   */
  static bool validate(StringPiece ip) noexcept;

  /**
   * Create an IPAddressV4 instance from a uint32_t, using network byte
   * order
   */
  static IPAddressV4 fromLong(uint32_t src);
  /**
   * Create an IPAddressV4 instance from a uint32_t, using host byte
   * order
   */
  static IPAddressV4 fromLongHBO(uint32_t src);

  /**
   * Create a new IPAddressV4 from the provided ByteRange.
   *
   * @throws IPAddressFormatException if the input length is not 4 bytes.
   */
  static IPAddressV4 fromBinary(ByteRange bytes);

  /**
   * Create a new IPAddressV4 from the provided ByteRange.
   *
   * Returns an IPAddressFormatError if the input length is not 4 bytes.
   */
  static Expected<IPAddressV4, IPAddressFormatError> tryFromBinary(
      ByteRange bytes) noexcept;

  /**
   * Create a new IPAddressV4 from the provided string.
   *
   * Returns an IPAddressFormatError if the string is not a valid IP.
   */
  static Expected<IPAddressV4, IPAddressFormatError> tryFromString(
      StringPiece str) noexcept;

  /**
   * Returns the address as a ByteRange.
   */
  ByteRange toBinary() const {
    return ByteRange((const unsigned char*)&addr_.inAddr_.s_addr, 4);
  }

  /**
   * Create a new IPAddressV4 from a `in-addr.arpa` representation of an IP
   * address.
   *
   * @throws IPAddressFormatException if the input is not a valid in-addr.arpa
   * representation
   */
  static IPAddressV4 fromInverseArpaName(const std::string& arpaname);

  /**
   * Convert a IPv4 address string to a long, in network byte order.
   */

  static uint32_t toLong(StringPiece ip);

  /**
   * Convert a IPv4 address string to a long, in host byte order.
   *
   * This is slightly slower than toLong()
   */
  static uint32_t toLongHBO(StringPiece ip);

  /**
   * Default constructor for IPAddressV4.
   *
   * The address value will be 0.0.0.0
   */
  IPAddressV4();

  /**
   * Construct an IPAddressV4 from a string.
   *
   * @throws IPAddressFormatException if the string is not a valid IPv4
   * address.
   */
  explicit IPAddressV4(StringPiece addr);

  /**
   * Construct an IPAddressV4 from a ByteArray4, in network byte order.
   */
  explicit IPAddressV4(const ByteArray4& src) noexcept;

  /**
   * Construct an IPAddressV4 from an `in_addr` representation of an IPV4
   * address
   */
  explicit IPAddressV4(const in_addr src) noexcept;

  /**
   * Return the IPV6 mapped representation of the address.
   */
  IPAddressV6 createIPv6() const;

  /**
   * Return an IPV6 address in the format of a 6To4 address.
   */
  IPAddressV6 getIPv6For6To4() const;

  /**
   * Return the uint32_t representation of the address, in network byte order.
   */
  uint32_t toLong() const { return toAddr().s_addr; }

  /**
   * Return the uint32_t representation of the address, in host byte order.
   */
  uint32_t toLongHBO() const { return ntohl(toLong()); }

  /**
   * Returns the number of bits in the IP address.
   *
   * @returns 32
   */
  static constexpr size_t bitCount() { return 32; }

  /**
   * Get a json representation of the IP address.
   *
   * This prints a string representation of the address, for human consumption
   * or logging. The string will take the form of a JSON object that looks like:
   *  `{family:'AF_INET', addr:'address', hash:long}`.
   */
  std::string toJson() const;

  /**
   * Returns a hash of the IP address.
   */
  size_t hash() const {
    static const uint32_t seed = AF_INET;
    uint32_t hashed = hash::fnv32_buf(&addr_, 4);
    return hash::hash_combine(seed, hashed);
  }

  /**
   * @overloadbrief Check if the IP address is found in the specified CIDR
   * netblock.
   *
   * @throws IPAddressFormatException if no /mask
   *
   * @note This is slower than the other inSubnet() overload. If perf is
   * important use the other overload, or inSubnetWithMask().
   * @param [in] cidrNetwork address in "192.168.1.0/24" format
   * @return true if address is part of specified subnet with cidr
   */
  bool inSubnet(StringPiece cidrNetwork) const;

  /**
   * Check if an IPAddressV4 belongs to a subnet.
   * @param [in] subnet Subnet to check against (e.g. 192.168.1.0)
   * @param [in] cidr   CIDR for subnet (e.g. 24 for /24)
   * @return true if address is part of specified subnet with cidr
   */
  bool inSubnet(const IPAddressV4& subnet, uint8_t cidr) const {
    return inSubnetWithMask(subnet, fetchMask(cidr));
  }

  /**
   * Check if an IPAddressV4 belongs to the subnet with the given mask.
   *
   * This is the same as inSubnet but the mask is provided instead of looked up
   * from the cidr.
   * @param [in] subnet Subnet to check against
   * @param [in] mask   The netmask for the subnet
   * @return true if address is part of the specified subnet with mask
   */
  bool inSubnetWithMask(const IPAddressV4& subnet, const ByteArray4 mask) const;

  /**
   * Return true if the IP address qualifies as localhost.
   */
  bool isLoopback() const;

  /**
   * Return true if the IP address qualifies as link local
   */
  bool isLinkLocal() const;

  /**
   * Return true if the IP address is a special purpose address, as defined per
   * RFC 6890 (i.e. 0.0.0.0).
   *
   */
  bool isNonroutable() const;
  /**
   * Return true if the IP address is private, as per RFC 1918 and RFC 4193.
   *
   * For example, 192.168.xxx.xxx
   */
  bool isPrivate() const;

  /**
   * Return true if the IP address is a multicast address.
   */
  bool isMulticast() const;

  /**
   * Returns true if the address is all zeros
   */
  bool isZero() const {
    constexpr auto zero = ByteArray4{{}};
    return 0 == std::memcmp(bytes(), zero.data(), zero.size());
  }

  /**
   * Return true if the IP address qualifies as broadcast.
   */
  bool isLinkLocalBroadcast() const {
    return (INADDR_BROADCAST == toLongHBO());
  }

  /**
   * Creates an IPAddressV4 with all but most significant numBits set to
   * 0.
   *
   * @throws IPAddressFormatException if numBits > bitCount()
   *
   * @param [in] numBits number of bits to mask
   * @return IPAddress instance with bits set to 0
   */
  IPAddressV4 mask(size_t numBits) const;

  /**
   * Provides a string representation of address.
   *
   * @throws if IPAddressFormatException on `inet_ntop` error.
   *
   * The string representation is calculated on demand.
   */
  std::string str() const;

  /**
   * Create the inverse arpa representation of the IP address.
   *
   */
  std::string toInverseArpaName() const;

  /**
   * Return the underlying `in_addr` structure
   */
  in_addr toAddr() const { return addr_.inAddr_; }

  /**
   * Return the IP address represented as a `sockaddr_in` struct
   *
   */
  sockaddr_in toSockAddr() const {
    sockaddr_in addr;
    memset(&addr, 0, sizeof(sockaddr_in));
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr, &addr_.inAddr_, sizeof(in_addr));
    return addr;
  }

  /**
   * Return a ByteArray4 containing the bytes of the IP address.
   */
  ByteArray4 toByteArray() const {
    ByteArray4 ba{{0}};
    std::memcpy(ba.data(), bytes(), 4);
    return ba;
  }

  /**
   * Return the fully qualified string representation of the address.
   *
   * This is the same as calling str().
   */
  std::string toFullyQualified() const { return str(); }

  /**
   * Same as toFullyQualified() but append to an output string.
   */
  void toFullyQualifiedAppend(std::string& out) const;

  /**
   * Returns the version of the IP Address (4).
   */
  uint8_t version() const { return 4; }

  /**
   * Return the mask associated with the given number of bits.
   *
   * If for instance numBits was 24 (e.g. /24) then the V4 mask returned should
   * be {0xff, 0xff, 0xff, 0x00}.
   *
   * @param [in] numBits bitmask to retrieve
   * @throws abort if numBits == 0 or numBits > bitCount()
   * @return mask associated with numBits
   */
  static ByteArray4 fetchMask(size_t numBits);

  /**
   * Given 2 (IPAddressV4, mask) pairs extract the longest common (IPAddressV4,
   * mask) pair
   */
  static CIDRNetworkV4 longestCommonPrefix(
      const CIDRNetworkV4& one, const CIDRNetworkV4& two);

  /**
   * Return the number of bytes in the IP address.
   *
   * @returns 4
   */
  static size_t byteCount() { return 4; }

  /**
   * Get the nth most significant bit of the IP address (0-indexed).
   * @param bitIndex n
   */
  bool getNthMSBit(size_t bitIndex) const {
    return detail::getNthMSBitImpl(*this, bitIndex, AF_INET);
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
  const unsigned char* bytes() const { return addr_.bytes_.data(); }

 private:
  union AddressStorage {
    static_assert(
        sizeof(in_addr) == sizeof(ByteArray4),
        "size of in_addr and ByteArray4 are different");
    in_addr inAddr_;
    ByteArray4 bytes_;
    AddressStorage() { std::memset(this, 0, sizeof(AddressStorage)); }
    explicit AddressStorage(const ByteArray4 bytes) : bytes_(bytes) {}
    explicit AddressStorage(const in_addr addr) : inAddr_(addr) {}
  } addr_;

  /**
   * Set the current IPAddressV4 object to the address specified by the
   * ByteRange given, in network byte order.
   *
   * Returns IPAddressFormatError if bytes.size() is not 4.
   */
  Expected<Unit, IPAddressFormatError> trySetFromBinary(
      ByteRange bytes) noexcept;
};

/**
 * `boost::hash` uses hash_value() so this allows `boost::hash` to work
 * automatically for IPAddressV4
 */
size_t hash_value(const IPAddressV4& addr);

/**
 * Appends a string representation of the IP address to the stream using str().
 */
std::ostream& operator<<(std::ostream& os, const IPAddressV4& addr);

/**
 * @overloadbrief Define toAppend() to allow IPAddress to be used with
 * `folly::to<string>`
 */
void toAppend(IPAddressV4 addr, std::string* result);
void toAppend(IPAddressV4 addr, fbstring* result);

/**
 * Return true if two addresses are equal.
 */
inline bool operator==(const IPAddressV4& addr1, const IPAddressV4& addr2) {
  return (addr1.toLong() == addr2.toLong());
}

/**
 * Three-way comparison for IPAddressV4.
 */
inline auto operator<=>(const IPAddressV4& a, const IPAddressV4& b) noexcept {
  return a.toLongHBO() <=> b.toLongHBO();
}

} // namespace folly

namespace std {
template <>
struct hash<folly::IPAddressV4> {
  size_t operator()(const folly::IPAddressV4 addr) const { return addr.hash(); }
};
} // namespace std
