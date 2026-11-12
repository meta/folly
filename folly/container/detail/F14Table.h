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

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <array>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/Bits.h>
#include <folly/ConstexprMath.h>
#include <folly/Likely.h>
#include <folly/Memory.h>
#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/Traits.h>
#include <folly/functional/Invoke.h>
#include <folly/lang/Align.h>
#include <folly/lang/Assume.h>
#include <folly/lang/Exception.h>
#include <folly/lang/Pretty.h>
#include <folly/lang/SafeAssert.h>
#include <folly/portability/Builtins.h>

#include <folly/container/HeterogeneousAccess.h>
#include <folly/container/detail/F14Defaults.h>
#include <folly/container/detail/F14IntrinsicsAvailability.h>
#include <folly/container/detail/F14Mask.h>

#if FOLLY_ARM_FEATURE_NEON_SVE_BRIDGE
#include <arm_neon_sve_bridge.h> // @manual
#include <arm_sve.h>
#endif

#if __has_include(<concepts>)
#include <concepts>
#endif

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

#if FOLLY_F14_CRC_INTRINSIC_AVAILABLE
#if FOLLY_NEON
#include <arm_acle.h> // __crc32cd
#else
#include <nmmintrin.h> // _mm_crc32_u64
#endif
#else
#ifdef _WIN32
#include <intrin.h> // _mul128 in fallback bit mixer
#endif
#endif

#if FOLLY_NEON
#include <arm_neon.h> // uint8x16t intrinsics
#if FOLLY_ARM_FEATURE_NEON_SVE_BRIDGE
#include <arm_neon_sve_bridge.h> // @manual
#include <arm_sve.h>
#endif
#elif FOLLY_SSE >= 2 // SSE2
#include <emmintrin.h> // _mm_set1_epi8
#include <immintrin.h> // __m128i intrinsics
#include <xmmintrin.h> // _mm_prefetch
#endif

#ifndef FOLLY_F14_PERTURB_INSERTION_ORDER
#define FOLLY_F14_PERTURB_INSERTION_ORDER folly::kIsDebug
#endif

#else // FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

#ifndef FOLLY_F14_PERTURB_INSERTION_ORDER
#define FOLLY_F14_PERTURB_INSERTION_ORDER false
#endif

#endif // FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

// On SVE2, derive the matching slot index straight from the svmatch predicate
// (BRKB + CNTP) instead of rebuilding a NEON nibble mask. Keyed off SVE2
// because svmatch is the SVE2-only intrinsic involved; the rest of the sequence
// is SVE1.
// This picks firstEmpty()'s return type; see the rule at
// FOLLY_F14_DETAIL_ISA_SVE2_WITH_BRIDGE in F14Mask.h.
#if FOLLY_NEON && FOLLY_ARM_FEATURE_NEON_SVE_BRIDGE && FOLLY_ARM_FEATURE_SVE2
#define FOLLY_F14_SVE_PREDICATE_NATIVE_ACTIVE 1
#else
#define FOLLY_F14_SVE_PREDICATE_NATIVE_ACTIVE 0
#endif

// For F14 functions with svbool_t/svuint8_t locals. At -O0, force-inlining
// them puts scalable allocas into a calling coroutine's body, which clang's
// CoroSplit cannot size ("Invalid size request on a scalable vector"). Only
// the inlining may depend on the optimization level: the ISA path must be the
// same in every TU, because the SVE2 and NEON paths differ in signatures (e.g.
// firstEmpty()'s return type) under the same mangled names.
#if defined(__OPTIMIZE__)
#define FOLLY_F14_SVE_INLINE FOLLY_ALWAYS_INLINE
#else
#define FOLLY_F14_SVE_INLINE FOLLY_NOINLINE
#endif

namespace folly {

struct F14TableStats {
  char const* policy;
  std::size_t size{0};
  std::size_t valueSize{0};
  std::size_t bucketCount{0};
  std::size_t chunkCount{0};
  std::vector<std::size_t> chunkOccupancyHisto;
  std::vector<std::size_t> chunkOutboundOverflowHisto;
  std::vector<std::size_t> chunkHostedOverflowHisto;
  std::vector<std::size_t> keyProbeLengthHisto;
  std::vector<std::size_t> missProbeLengthHisto;
  std::size_t totalBytes{0};
  std::size_t overheadBytes{0};

 private:
  template <typename T>
  static auto computeHelper(T const* m) -> decltype(m->computeStats()) {
    return m->computeStats();
  }

  static F14TableStats computeHelper(...) { return {}; }

 public:
  template <typename T>
  static F14TableStats compute(T const& m) {
    return computeHelper(&m);
  }
};

namespace f14 {
namespace detail {

template <F14IntrinsicsMode>
struct F14LinkCheck {};

template <>
struct F14LinkCheck<getF14IntrinsicsMode()> {
  // The purpose of this method is to trigger a link failure if
  // compilation flags vary across compilation units.  The definition
  // is in F14Table.cpp, so only one of F14LinkCheck<None>::check,
  // F14LinkCheck<Simd>::check, or F14LinkCheck<SimdAndCrc>::check will
  // be available at link time.
  //
  // To cause a link failure the function must be invoked in code that
  // is not optimized away, so we call it on a couple of cold paths
  // (exception handling paths in copy construction and rehash).  LTO may
  // remove it entirely, but that's fine.
  static void check() noexcept;
};

bool tlsPendingSafeInserts(std::ptrdiff_t delta = 0);
std::size_t tlsMinstdRand(std::size_t n);

#if defined(_LIBCPP_VERSION)

template <typename K, typename V, typename H>
struct StdNodeReplica {
  void* next;
  std::size_t hash;
  V value;
};

#elif defined(__GLIBCXX__)

template <typename K, typename H>
constexpr bool kStdNodeContainsHash =
    !std::__is_fast_hash<H>::value || !is_nothrow_invocable_v<H, K const&>;

// mimic internal node of unordered containers in STL to estimate the size
template <typename K, typename V, typename H, typename Enable = void>
struct StdNodeReplica {
  void* next;
  V value;
};
template <typename K, typename V, typename H>
struct StdNodeReplica<K, V, H, std::enable_if_t<kStdNodeContainsHash<K, H>>> {
  void* next;
  V value;
  std::size_t hash;
};

#else

template <typename K, typename V, typename H>
struct StdNodeReplica {
  void* next;
  V value;
};

#endif

template <class Container, class Predicate>
typename Container::size_type erase_if_impl(
    Container& c, Predicate& predicate) {
  auto const old_size = c.size();
  for (auto i = c.begin(), last = c.end(); i != last;) {
    auto prev = i++;
    if (predicate(*prev)) {
      c.erase(prev);
    }
  }
  return old_size - c.size();
}

} // namespace detail
} // namespace f14

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
namespace f14 {
namespace detail {
template <typename Policy>
class F14Table;
} // namespace detail
} // namespace f14

class F14HashToken final {
 public:
  constexpr F14HashToken() = default;

  friend constexpr bool operator==(
      F14HashToken const& a, F14HashToken const& b) noexcept {
    return a.hp_.first == b.hp_.first; // processed hash but not tag
  }
  friend constexpr bool operator!=(
      F14HashToken const& a, F14HashToken const& b) noexcept {
    return !(a == b);
  }

 private:
  using HashPair = std::pair<std::size_t, std::size_t>;

  constexpr explicit F14HashToken(HashPair hp) noexcept : hp_(hp) {}
  constexpr explicit operator HashPair() const noexcept { return hp_; }

  HashPair hp_;

  template <typename Policy>
  friend class f14::detail::F14Table;

  template <typename Key, typename Hasher, typename KeyEqual>
  friend class F14HashedKey;
};

#else
class F14HashToken final {
  friend constexpr bool operator==(
      F14HashToken const&, F14HashToken const&) noexcept {
    return true;
  }
  friend constexpr bool operator!=(
      F14HashToken const&, F14HashToken const&) noexcept {
    return false;
  }
};
#endif

static_assert(std::regular<F14HashToken>);

namespace f14 {
namespace detail {

// Detection for folly_assume_32bit_hash

template <typename Hasher, typename Void = void>
struct ShouldAssume32BitHash : std::bool_constant<!require_sizeof<Hasher>> {};

template <typename Hasher>
struct ShouldAssume32BitHash<
    Hasher,
    std::void_t<typename Hasher::folly_assume_32bit_hash>>
    : std::bool_constant<Hasher::folly_assume_32bit_hash::value> {};

//////// hash helpers

// Hash values are used to compute the desired position, which is the
// chunk index at which we would like to place a value (if there is no
// overflow), and the tag, which is an additional 8 bits of entropy.
//
// The standard's definition of hash function quality only refers to
// the probability of collisions of the entire hash value, not to the
// probability of collisions of the results of shifting or masking the
// hash value.  Some hash functions, however, provide this stronger
// guarantee (not quite the same as the definition of avalanching,
// but similar).
//
// If the user-supplied hasher is an avalanching one (each bit of the
// hash value has a 50% chance of being the same for differing hash
// inputs), then we can just take 8 bits of the hash value for the tag
// and the rest for the desired position.  Avalanching hashers also
// let us map hash value to array index position with just a bitmask
// without risking clumping.  (Many hash tables just accept the risk
// and do it regardless.)
//
// std::hash<std::string> avalanches in all implementations we've
// examined: libstdc++-v3 uses MurmurHash2, and libc++ uses CityHash
// or MurmurHash2.  The other std::hash specializations, however, do not
// have this property.  std::hash for integral and pointer values is the
// identity function on libstdc++-v3 and libc++, in particular.  In our
// experience it is also fairly common for user-defined specializations
// of std::hash to combine fields in an ad-hoc way that does not evenly
// distribute entropy among the bits of the result (a + 37 * b, for
// example, where a and b are integer fields).
//
// For hash functions we don't trust to avalanche, we repair things by
// applying a bit mixer to the user-supplied hash.

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
#if FOLLY_X64 || FOLLY_AARCH64 || FOLLY_RISCV64 || FOLLY_WASM64
// 64-bit
template <typename Hasher, typename Key>
std::pair<std::size_t, std::size_t> splitHashImpl(std::size_t hash) {
  static_assert(sizeof(std::size_t) == sizeof(uint64_t));
  std::size_t tag;
  if (!IsAvalanchingHasher<Hasher, Key>::value) {
#if FOLLY_F14_CRC_INTRINSIC_AVAILABLE
#if FOLLY_SSE_PREREQ(4, 2)
    // SSE4.2 CRC
    std::size_t c = _mm_crc32_u64(0, hash);
    tag = (c >> 24);
    hash += c;
#else
    // CRC is optional on armv8 (-march=armv8-a+crc), standard on armv8.1
    std::size_t c = __crc32cd(0, hash);
    tag = (c >> 24);
    hash += c;
#endif
#else
    // The mixer below is not fully avalanching for all 64 bits of
    // output, but looks quite good for bits 18..63 and puts plenty
    // of entropy even lower when considering multiple bits together
    // (like the tag).  Importantly, when under register pressure it
    // uses fewer registers, instructions, and immediate constants
    // than the alternatives, resulting in compact code that is more
    // easily inlinable.  In one instantiation a modified Murmur mixer
    // was 48 bytes of assembly (even after using the same multiplicand
    // for both steps) and this one was 27 bytes, for example.
    auto const kMul = 0xc4ceb9fe1a85ec53ULL;
#if defined(_WIN32) && defined(_M_X64)
    __int64 signedHi;
    __int64 signedLo = _mul128(
        static_cast<__int64>(hash), static_cast<__int64>(kMul), &signedHi);
    auto hi = static_cast<uint64_t>(signedHi);
    auto lo = static_cast<uint64_t>(signedLo);
#elif defined(_MSC_VER) && !defined(__clang__)
    auto hi = __umulh(hash, kMul);
    auto lo = hash * kMul;
#else
    auto hi = static_cast<uint64_t>(
        (static_cast<unsigned __int128>(hash) * kMul) >> 64);
    auto lo = hash * kMul;
#endif
    hash = hi ^ lo;
    hash *= kMul;
    tag = ((hash >> 14) & 0xff);
    hash >>= 22;
#endif
  } else {
    // F14 uses the bottom bits of the hash to form the index, so for maps
    // with less than 16.7 million entries, it's safe to have a 32-bit hash,
    // and use the bottom 24 bits for the index and leave the top 8 for the
    // tag.
    //
    // We need to avoid 0 tag for a non-empty value.
    if (ShouldAssume32BitHash<Hasher>::value) {
      tag = (hash >> 24) & 0xFF;
      // Explicitly mask off the top 32-bits so that the compiler can
      // optimize away whatever is populating the top 32-bits, which is likely
      // just the lower 32-bits duplicated.
      hash = hash & 0xFFFF'FFFF;
    } else {
      tag = (hash >> 56);
    }
  }
  return std::make_pair(hash, std::max(std::size_t{1}, tag));
}
#else
// 32-bit
template <typename Hasher, typename Key>
std::pair<std::size_t, std::size_t> splitHashImpl(std::size_t hash) {
  static_assert(sizeof(std::size_t) == sizeof(uint32_t));
  uint8_t tag;
  if (!IsAvalanchingHasher<Hasher, Key>::value) {
#if FOLLY_F14_CRC_INTRINSIC_AVAILABLE
#if FOLLY_SSE_PREREQ(4, 2)
    // SSE4.2 CRC
    auto c = _mm_crc32_u32(0, hash);
    tag = static_cast<uint8_t>(c >> 24);
    hash += c;
#else
    auto c = __crc32cw(0, hash);
    tag = static_cast<uint8_t>(c >> 24);
    hash += c;
#endif
#else
    // finalizer for 32-bit murmur2
    hash ^= hash >> 13;
    hash *= 0x5bd1e995;
    hash ^= hash >> 15;
    tag = static_cast<uint8_t>(hash >> 24);
#endif
  } else {
    // We need to avoid 0 tag for a non-empty value.
    tag = (hash >> 24);
  }
  return std::make_pair(hash, std::max<std::size_t>(1, tag));
}
#endif
#endif
} // namespace detail
} // namespace f14

template <
    typename TKeyType,
    typename Hasher = f14::DefaultHasher<TKeyType>,
    typename KeyEqual = f14::DefaultKeyEqual<TKeyType>>
class F14HashedKey final {
 private:
  template <typename K>
  using EligibleForHeterogeneousCompare =
      detail::EligibleForHeterogeneousFind<TKeyType, Hasher, KeyEqual, K>;

  template <typename K, typename T>
  using EnableHeterogeneousCompare =
      std::enable_if_t<EligibleForHeterogeneousCompare<K>::value, T>;

  static constexpr void checkTemplateParamContract() {
    static_assert(is_constexpr_default_constructible_v<Hasher>);
    static_assert(is_constexpr_default_constructible_v<KeyEqual>);
    static_assert(std::is_trivially_copyable_v<Hasher>);
    static_assert(std::is_trivially_copyable_v<KeyEqual>);
    static_assert(std::is_empty_v<Hasher>);
    static_assert(std::is_empty_v<KeyEqual>);
    // When `Hasher` or `KeyEqual` is not transparent, `F14HashedKey` will
    // behave like `TKeyType` without any performance effect, it is most likely
    // not what is expected.
    static_assert(is_transparent_v<Hasher>);
    static_assert(is_transparent_v<KeyEqual>);
  }

 public:
#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
  template <typename... Args>
  explicit F14HashedKey(Args&&... args)
      : key_(std::forward<Args>(args)...),
        hash_(f14::detail::splitHashImpl<Hasher, TKeyType>(Hasher{}(key_))) {
    checkTemplateParamContract();
  }
#else
  F14HashedKey() = delete;
#endif

  const TKeyType& getKey() const { return key_; }
  const F14HashToken& getHashToken() const { return hash_; }
  // We want the conversion to the key to be implicit - the hashed key should
  // seamlessly behave as the key itself.
  /* implicit */ operator const TKeyType&() const { return key_; }
  explicit operator const F14HashToken&() const { return hash_; }

  template <typename T>
  using IsRangeConvertible =
      std::enable_if_t<detail::TransparentlyConvertibleToRange<T>::value>;

  template <typename T>
  using RangeT =
      Range<typename detail::ValueTypeForTransparentConversionToRange<
          T>::type const*>;

  template <typename K = TKeyType, typename Enable = IsRangeConvertible<K>>
  constexpr explicit operator RangeT<K>() const {
    return key_;
  }

  friend bool operator==(const F14HashedKey& a, const F14HashedKey& b) {
    return KeyEqual{}(a.key_, b.key_);
  }
  friend bool operator!=(const F14HashedKey& a, const F14HashedKey& b) {
    return !(a == b);
  }
  friend bool operator==(const F14HashedKey& a, const TKeyType& b) {
    return KeyEqual{}(a.key_, b);
  }
  friend bool operator!=(const F14HashedKey& a, const TKeyType& b) {
    return !(a == b);
  }
  friend bool operator==(const TKeyType& a, const F14HashedKey& b) {
    return KeyEqual{}(a, b.key_);
  }
  friend bool operator!=(const TKeyType& a, const F14HashedKey& b) {
    return !(a == b);
  }
  template <typename K>
  friend EnableHeterogeneousCompare<K, bool> operator==(
      const F14HashedKey& a, const K& b) {
    return KeyEqual{}(a.key_, b);
  }
  template <typename K>
  friend EnableHeterogeneousCompare<K, bool> operator!=(
      const F14HashedKey& a, const K& b) {
    return !(a == b);
  }
  template <typename K>
  friend EnableHeterogeneousCompare<K, bool> operator==(
      const K& a, const F14HashedKey& b) {
    return KeyEqual{}(a, b.key_);
  }
  template <typename K>
  friend EnableHeterogeneousCompare<K, bool> operator!=(
      const K& a, const F14HashedKey& b) {
    return !(a == b);
  }

 private:
  TKeyType key_;
  F14HashToken hash_;
};

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE
namespace f14 {
namespace detail {

//// Defaults should be selected using void
template <typename Arg, typename Default>
using VoidDefault =
    std::conditional_t<std::is_same<Arg, Default>::value, void, Arg>;

template <typename Arg, typename Default>
using Defaulted =
    std::conditional_t<std::is_same<Arg, void>::value, Default, Arg>;

////////////////

/// Prefetch the first cache line of the object at ptr.
template <typename T>
FOLLY_ALWAYS_INLINE static void prefetchAddr(T const* ptr) {
#ifndef _WIN32
  FOLLY_PUSH_WARNING
  FOLLY_GNU_DISABLE_WARNING("-Warray-bounds")
  /// The argument ptr is permitted to be wild, since wild pointers are allowed
  /// for prefetching on every architecture. While this behavior is technically
  /// undefined (forbidden) in C and C++, we need this behavior in order to
  /// avoid extra cost in the callers. Recent versions of GCC warn when they
  /// detect uses of pointers which may be wild. So we suppress the warning.
  __builtin_prefetch(static_cast<void const*>(ptr));
  FOLLY_POP_WARNING
#elif FOLLY_NEON
  __prefetch(static_cast<void const*>(ptr));
#elif FOLLY_SSE >= 2
  _mm_prefetch(
      static_cast<char const*>(static_cast<void const*>(ptr)), _MM_HINT_T0);
#endif
}

#if FOLLY_NEON
using TagVector = uint8x16_t;
#elif FOLLY_SSE >= 2
using TagVector = __m128i;
#elif FOLLY_HAVE_INT128_T
using TagVector = __uint128_t;
#endif

// We could use unaligned loads to relax this requirement, but that
// would be both a performance penalty and require a bulkier packed
// ItemIter format
constexpr std::size_t kRequiredVectorAlignment =
    constexpr_max(std::size_t{16}, alignof(max_align_t));

struct alignas(kRequiredVectorAlignment) F14ChunkHeader {
  static constexpr std::uint8_t kOutboundOverflowMax = 254;
  static constexpr std::uint8_t kOutboundOverflowEmpty = 255;

  // Zero means an empty tag. tags_ array might be bigger
  // than kCapacity to keep alignment of first item.
  std::array<uint8_t, 14> tags_;

  // Bits 0..3 of chunk 0 record the scaling factor between the number of
  // chunks and the max size without rehash.  Bits 4-7 in any chunk are a
  // 4-bit counter of the number of values in this chunk that were placed
  // because they overflowed their desired chunk (hostedOverflowCount).
  uint8_t control_;

  // The number of values that would have been placed into this chunk if
  // there had been space, including values that also overflowed previous
  // full chunks.  This value saturates; once it becomes 254 it no longer
  // increases nor decreases.
  uint8_t outboundOverflowCount_;
};

inline constexpr F14ChunkHeader kF14EmptyChunkHeader{
    {}, 0, F14ChunkHeader::kOutboundOverflowEmpty};

template <typename ItemType>
struct alignas(constexpr_max(kRequiredVectorAlignment, alignof(ItemType)))
    F14Chunk : F14ChunkHeader {
  using Item = ItemType;

  // Alignment required for chunks, which must satisfy both the SIMD tag
  // vector alignment and the stored item alignment.
  static constexpr std::size_t kRequiredChunkAlignment =
      constexpr_max(kRequiredVectorAlignment, alignof(Item));

  // For our 16 byte vector alignment (and assuming alignof(Item) >=
  // 4) kCapacity of 14 is the most space efficient.  Slightly smaller
  // or larger capacities can help with cache alignment in a couple of
  // cases without wasting too much space, but once the items are larger
  // then we're unlikely to get much benefit anyway.  The only case we
  // optimize is using kCapacity of 12 for 4 byte items, which makes the
  // chunk take exactly 1 cache line, and adding 16 bytes of padding for
  // 16 byte items so that a chunk takes exactly 4 cache lines.
  static constexpr unsigned kCapacity = sizeof(Item) == 4 ? 12 : 14;

  static constexpr unsigned kDesiredCapacity = kCapacity - 2;

  static constexpr unsigned kAllocatedCapacity =
      kCapacity + (sizeof(Item) == 16 ? 1 : 0);

  // If kCapacity == 12 then we get 16 bits of capacityScale by using
  // tag 12 and 13, otherwise we only get 4 bits of control_
  static constexpr std::size_t kCapacityScaleBits = kCapacity == 12 ? 16 : 4;
  static constexpr std::size_t kCapacityScaleShift = kCapacityScaleBits - 4;

  static constexpr MaskType kFullMask = FullMask<kCapacity>::value;

  // Items are stored at a fixed byte offset from the start of the chunk,
  // accessed via pointer arithmetic rather than a member array.  This keeps
  // sizeof(F14Chunk) small (header only), which satisfies UBSAN's
  // object-size check even for single-chunk tables that allocate fewer
  // than kAllocatedCapacity items.  The offset is the chunk alignment to
  // ensure items are properly aligned.
  static constexpr std::size_t kItemsOffset = kRequiredChunkAlignment;

  // Stride between consecutive chunks in a multi-chunk allocation.
  // This equals the header (sizeof(F14Chunk)) + items, rounded up
  // to alignment.
  static constexpr std::size_t kChunkStride = align_ceil(
      kItemsOffset + kAllocatedCapacity * sizeof(Item),
      kRequiredChunkAlignment);

  // Navigate between chunks using kChunkStride instead of sizeof(F14Chunk).
 private:
  template <typename ChunkType>
  static ChunkType* chunkRawAtImpl(ChunkType* base, std::size_t i) {
    auto* bytePtr = reinterpret_cast<char*>(base);
    auto offset = static_cast<std::ptrdiff_t>(i) *
        static_cast<std::ptrdiff_t>(kChunkStride);
    return reinterpret_cast<ChunkType*>(bytePtr + offset);
  }

  template <typename ChunkType>
  static ChunkType* chunkRawOffsetImpl(ChunkType* c, std::ptrdiff_t delta) {
    auto* bytePtr = reinterpret_cast<char*>(c);
    auto offset = delta * static_cast<std::ptrdiff_t>(kChunkStride);
    return reinterpret_cast<ChunkType*>(bytePtr + offset);
  }

 public:
  static F14Chunk* chunkRawAt(F14Chunk* base, std::size_t i) {
    return chunkRawAtImpl(base, i);
  }
  static F14Chunk const* chunkRawAt(F14Chunk const* base, std::size_t i) {
    return chunkRawAtImpl(base, i);
  }

  static F14Chunk* prevChunkRaw(F14Chunk* c) {
    return chunkRawOffsetImpl(c, -1);
  }
  static F14Chunk const* prevChunkRaw(F14Chunk const* c) {
    return chunkRawOffsetImpl(c, -1);
  }

  static F14Chunk* nextChunkRaw(F14Chunk* c) {
    return chunkRawOffsetImpl(c, 1);
  }
  static F14Chunk const* nextChunkRaw(F14Chunk const* c) {
    return chunkRawOffsetImpl(c, 1);
  }

  static std::size_t chunkRawIndex(
      F14Chunk const* base, F14Chunk const* target) {
    auto const* baseBytes = reinterpret_cast<char const*>(base);
    auto const* targetBytes = reinterpret_cast<char const*>(target);
    auto const byteOffset = targetBytes - baseBytes;
    return static_cast<std::size_t>(byteOffset) / kChunkStride;
  }

  static constexpr F14ChunkHeader* getSomeEmptyInstance() noexcept {
    return const_cast<F14ChunkHeader*>(&kF14EmptyChunkHeader);
  }

  static bool isEmptyInstance(F14Chunk const* const chunk) noexcept {
    return chunk->outboundOverflowCount_ == kOutboundOverflowEmpty;
  }

  void clear() {
    // tags_ = {}; control_ = 0; outboundOverflowCount_ = 0;

    // gcc < 6 doesn't exploit chunk alignment to generate the optimal
    // SSE clear from memset.  This is very hot code, so it is worth
    // handling that case specially.
    std::memset(&tags_[0], '\0', 16);
  }

  void copyOverflowInfoFrom(F14Chunk const& rhs) {
    FOLLY_SAFE_DCHECK(!isEmptyInstance(&rhs));
    FOLLY_SAFE_DCHECK(hostedOverflowCount() == 0, "");
    control_ += static_cast<uint8_t>(rhs.control_ & 0xf0);
    outboundOverflowCount_ = rhs.outboundOverflowCount_;
  }

  unsigned hostedOverflowCount() const { return control_ >> 4; }

  static constexpr uint8_t kIncrHostedOverflowCount = 0x10;
  static constexpr uint8_t kDecrHostedOverflowCount =
      static_cast<uint8_t>(-0x10);

  void adjustHostedOverflowCount(uint8_t op) { control_ += op; }

  bool eof() const { return capacityScale(this) != 0; }

  static std::size_t capacityScale(F14Chunk const* const chunk) {
    if constexpr (kCapacityScaleBits == 4) {
      return chunk->control_ & 0xf;
    } else {
      uint16_t v;
      std::memcpy(&v, &chunk->tags_[12], 2);
      return v;
    }
  }

  void setCapacityScale(std::size_t scale) {
    FOLLY_SAFE_DCHECK(
        !isEmptyInstance(this) && scale > 0 &&
            scale < (std::size_t{1} << kCapacityScaleBits),
        "");
    if constexpr (kCapacityScaleBits == 4) {
      control_ = static_cast<uint8_t>((control_ & ~0xf) | scale);
    } else {
      uint16_t v = static_cast<uint16_t>(scale);
      std::memcpy(&tags_[12], &v, 2);
    }
  }

  void markEof(std::size_t scale) {
    folly::assume(control_ == 0);
    setCapacityScale(scale);
  }

  unsigned outboundOverflowCount() const { return outboundOverflowCount_; }

  void incrOutboundOverflowCount() {
    if (outboundOverflowCount_ != kOutboundOverflowMax) {
      ++outboundOverflowCount_;
    }
  }

  void decrOutboundOverflowCount() {
    FOLLY_SAFE_DCHECK(outboundOverflowCount_ != 0);
    if (outboundOverflowCount_ != kOutboundOverflowMax) {
      --outboundOverflowCount_;
    }
  }

  std::size_t tag(std::size_t index) const { return tags_[index]; }

  void checkSetTag(std::size_t index, std::size_t tag) const {
    FOLLY_SAFE_DCHECK(
        !isEmptyInstance(this) && tag >= 1 && tag <= 0xff && index < kCapacity,
        "");
    FOLLY_SAFE_CHECK(tags_[index] == 0, "");
  }

  void setTag(std::size_t index, std::size_t tag) {
    checkSetTag(index, tag);
    tags_[index] = static_cast<uint8_t>(tag);
  }

  // Sets the tag of an empty slot given w, the header as it is in memory, so
  // one load serves both the search and the store.  On the vector backends
  // setTagImpl() rewrites all 16 header bytes rather than storing the one,
  // which avoids a store-forwarding stall if the header is reloaded shortly
  // after.  Callers with no word in hand keep using setTag() above; loading
  // the header just to rewrite it would be slower.
  void setTag(TagVector w, std::size_t index, std::size_t tag) {
    checkSetTag(index, tag);
    FOLLY_SAFE_DCHECK(std::memcmp(&w, &tags_[0], sizeof(w)) == 0, "");
    setTagImpl(w, index, tag);
  }

  void clearTag(std::size_t index) {
    FOLLY_SAFE_CHECK(tags_[index] != 0, "");
    tags_[index] = 0;
  }

#if FOLLY_NEON

  ////////
  // Tag filtering using NEON/SVE intrinsics

#if FOLLY_F14_DETAIL_ISA_SVE2_WITH_BRIDGE

  // Returns the matching-tag iterator together with a fast "any match" flag
  // taken straight from the SVE predicate register, which lets the caller
  // resolve the common no-match branch without waiting on the mask extraction.
  std::pair<SparseMaskIter, bool> tagMatchIter(uint8x16_t needleV) const {
    svbool_t pred = svwhilelt_b8_u32(0, kCapacity);
    // Plain-NEON 16-byte load v.s. SVE predicated 12/14-byte load: some of the
    // remaining bytes will be needed for outboundOverflowCount(). If they are
    // already in a register from a 16-byte load, the optimizer will get these
    // bytes from the register and avoid issuing an additional scalar load.
    svuint8_t tagV = svset_neonq_u8(svundef_u8(), vld1q_u8(&tags_[0]));
    svbool_t matchPred =
        svmatch_u8(pred, tagV, svset_neonq_u8(svundef_u8(), needleV));
    // get info from every byte into the bottom half of every uint16_t
    // by shifting right 4, then round to get it into a 64-bit vector
    uint8x8_t maskV = vshrn_n_u16(
        vreinterpretq_u16_u8(svget_neonq(svdup_n_u8_z(matchPred, 17))), 4);
    uint64_t mask = vreinterpret_u64_u8(maskV)[0];
    return {SparseMaskIter(mask), svptest_any(pred, matchPred)};
  }

  // Visits the index of each slot whose tag matches, stopping early if func
  // returns true; the return value is whether it stopped early.
  //
  // svbool_t is a sizeless type, so it cannot be stored in an iterator object
  // and handed back to the caller. Inverting the loop keeps the predicate in a
  // register, which lets the match index be derived without the round trip
  // through a NEON nibble mask that tagMatchIter above has to perform.
  template <typename F>
  FOLLY_F14_SVE_INLINE bool forEachTagMatch(uint8x16_t needleV, F func) const {
    svbool_t pred = svwhilelt_b8_u32(0, kCapacity);
    // Plain-NEON 16-byte load v.s. SVE predicated 12/14-byte load: see matching
    // line in tagMatchIter().
    svuint8_t tagV = svset_neonq_u8(svundef_u8(), vld1q_u8(&tags_[0]));
    svbool_t rem =
        svmatch_u8(pred, tagV, svset_neonq_u8(svundef_u8(), needleV));
    // svmatch and svbic both set the condition flags, so this test is free.
    while (svptest_any(pred, rem)) {
      // BRKB sets the active lanes preceding the first match, so counting them
      // yields that match's index.
      unsigned i = static_cast<unsigned>(svcntp_b8(pred, svbrkb_z(pred, rem)));
      FOLLY_SAFE_DCHECK(i < kCapacity, "");
      if (func(i)) {
        return true;
      }
      // Clear the visited lane: BRKA covers everything up to and including it.
      rem = svbic_b_z(pred, rem, svbrka_z(pred, rem));
    }
    return false;
  }

#endif

  TagVector loadWord() const { return vld1q_u8(&tags_[0]); }

  std::pair<SparseMaskIter, bool> tagMatchIter(
      TagVector w, uint8x16_t needleV) const {
    auto eqV = vceqq_u8(w, needleV);
    // get info from every byte into the bottom half of every uint16_t
    // by shifting right 4, then round to get it into a 64-bit vector
    uint8x8_t maskV = vshrn_n_u16(vreinterpretq_u16_u8(eqV), 4);
    uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(maskV), 0) & kFullMask;
    SparseMaskIter iter(mask);
    return {iter, iter.hasNext()};
  }

  MaskType occupiedMask(TagVector w) const {
    // Shortcut for w != 0.
    auto occupiedV = vtstq_u8(w, w);
    uint8x8_t maskV = vshrn_n_u16(vreinterpretq_u16_u8(occupiedV), 4);
    return vget_lane_u64(vreinterpret_u64_u8(maskV), 0) & kFullMask;
  }

  unsigned outboundOverflowCount(TagVector w) const {
    return vgetq_lane_u8(w, offsetof(F14ChunkHeader, outboundOverflowCount_));
  }

  void setTagImpl(TagVector w, std::size_t index, std::size_t tag) {
    static constexpr uint8_t kLaneIndex[16] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    auto laneV =
        vceqq_u8(vld1q_u8(kLaneIndex), vdupq_n_u8(static_cast<uint8_t>(index)));
    vst1q_u8(
        &tags_[0], vbslq_u8(laneV, vdupq_n_u8(static_cast<uint8_t>(tag)), w));
  }
#elif FOLLY_SSE >= 2
  ////////
  // Tag filtering using SSE2 intrinsics

  TagVector const* tagVector() const {
    return static_cast<TagVector const*>(static_cast<void const*>(&tags_[0]));
  }

  auto tagMatchIter(TagVector w, __m128i needleV) const {
    auto eqV = _mm_cmpeq_epi8(w, needleV);
    uint32_t mask = _mm_movemask_epi8(eqV);
    if constexpr (kIsArchAmd64) {
      BoundedMaskIter<kCapacity> iter{mask};
      return std::pair{iter, iter.hasNext()};
    } else {
      SparseMaskIter iter{mask & kFullMask};
      return std::pair{iter, iter.hasNext()};
    }
  }

  TagVector loadWord() const { return _mm_load_si128(tagVector()); }

  MaskType occupiedMask(TagVector w) const {
    auto emptyV = _mm_cmpeq_epi8(w, _mm_setzero_si128());
    return (~_mm_movemask_epi8(emptyV)) & kFullMask;
  }

  unsigned outboundOverflowCount(TagVector w) const {
    // The byte is the high half of the last 16-bit lane.
    return static_cast<unsigned>(_mm_extract_epi16(
               w, offsetof(F14ChunkHeader, outboundOverflowCount_) / 2)) >>
        8;
  }

  void setTagImpl(TagVector w, std::size_t index, std::size_t tag) {
    auto laneV = _mm_cmpeq_epi8(
        _mm_setr_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
        _mm_set1_epi8(static_cast<char>(index)));
    auto tagV = _mm_set1_epi8(static_cast<char>(tag));
#if FOLLY_SSE_PREREQ(4, 1)
    auto tagsV = _mm_blendv_epi8(w, tagV, laneV);
#else
    // The slot is empty, so splicing the tag in is an or.
    auto tagsV = _mm_or_si128(w, _mm_and_si128(laneV, tagV));
#endif
    _mm_store_si128(
        static_cast<TagVector*>(static_cast<void*>(&tags_[0])), tagsV);
  }
#elif FOLLY_HAVE_INT128_T
  ////////
  // Tag filtering using plain C/C++

  std::pair<SparseMaskIter, bool> tagMatchIter(
      TagVector, std::size_t needle) const {
    FOLLY_SAFE_DCHECK(needle >= 1 && needle < 0x100, "");
    auto tagV = static_cast<uint8_t const*>(&tags_[0]);
    MaskType mask = 0;
    FOLLY_PRAGMA_UNROLL_N(16)
    for (auto i = 0u; i < kCapacity; i++) {
      mask |= ((tagV[i] == static_cast<uint8_t>(needle)) ? 1 : 0) << i;
    }
    SparseMaskIter iter{mask & kFullMask};
    return {iter, iter.hasNext()};
  }

  TagVector loadWord() const {
    TagVector w;
    std::memcpy(&w, &tags_[0], sizeof(w));
    return w;
  }

  MaskType occupiedMask(TagVector) const {
    auto tagV = static_cast<uint8_t const*>(&tags_[0]);
    MaskType mask = 0;
    FOLLY_PRAGMA_UNROLL_N(16)
    for (auto i = 0u; i < kCapacity; i++) {
      mask |= (tagV[i] ? 1 : 0) << i;
    }
    return mask & kFullMask;
  }

  unsigned outboundOverflowCount(TagVector) const {
    return outboundOverflowCount();
  }

  void setTagImpl(TagVector, std::size_t index, std::size_t tag) {
    tags_[index] = static_cast<uint8_t>(tag);
  }
#endif

#if !FOLLY_F14_SVE_PREDICATE_NATIVE_ACTIVE

  // Visits the index of each slot whose tag matches, stopping early if func
  // returns true; the return value is whether it stopped early.
  template <typename N, typename F>
  FOLLY_ALWAYS_INLINE bool forEachTagMatch(N needleV, F func) const {
    auto [hits, nonzero] = tagMatchIter(loadWord(), needleV);
    if (nonzero) {
      do {
        if (func(hits.next())) {
          return true;
        }
      } while (hits.hasNext());
    }
    return false;
  }

#endif

  MaskType occupiedMask() const { return occupiedMask(loadWord()); }

  auto occupiedIter() const {
    if constexpr (kIsArchAArch64) {
      return SparseMaskIter{this->occupiedMask()};
    } else {
      return DenseMaskIter{&tags_[0], this->occupiedMask()};
    }
  }
  MaskRangeIter occupiedRangeIter() const {
    return MaskRangeIter{this->occupiedMask()};
  }

  LastOccupiedInMask lastOccupied() const {
    return LastOccupiedInMask{this->occupiedMask()};
  }

#if FOLLY_F14_SVE_PREDICATE_NATIVE_ACTIVE

  // Predicate-native, unlike occupiedMask() above (used by occupiedIter(),
  // occupiedRangeIter(), and lastOccupied()): building occupiedMask()'s
  // packed MaskType requires converting the predicate back into a vector
  // (mov z,p/z,#imm), which measured as a net loss on Neoverse-V2. Since
  // this call only needs a single index (or "none"), it can instead extract
  // one directly via BRKB+CNTP, the same technique forEachTagMatch uses.
  ResolvedFirstEmpty firstEmpty(TagVector w) const {
    svbool_t pred = svwhilelt_b8_u32(0, kCapacity);
    svuint8_t tagV = svset_neonq_u8(svundef_u8(), w);
    svbool_t emptyPred = svcmpeq_n_u8(pred, tagV, 0);
    if (!svptest_any(pred, emptyPred)) {
      return {false, 0};
    }
    std::size_t i = svcntp_b8(pred, svbrkb_z(pred, emptyPred));
    return {true, i};
  }

#else

  FirstEmptyInMask firstEmpty(TagVector w) const {
    return FirstEmptyInMask{this->occupiedMask(w) ^ kFullMask};
  }

#endif

  auto firstEmpty() const { return firstEmpty(loadWord()); }

  bool occupied(std::size_t index) const { return tags_[index] != 0; }

  /// Permitted to return a wild pointer, which is allowed for prefetching on
  /// every architecture. This behavior is technically undefined (forbidden) in
  /// C and C++, but we violate the rule in order to avoid extra cost in the
  /// prefetch paths. The wild pointer that may be returned is whatever follows
  /// any empty-instance global in the memory of any DSO.
  Item* itemAddr(std::size_t i) const {
    auto const* chunkBytes = reinterpret_cast<unsigned char const*>(this);
    auto const itemOffset = kItemsOffset + i * sizeof(Item);
    auto const* itemBytes = chunkBytes + itemOffset;
    return static_cast<Item*>(
        const_cast<void*>(static_cast<void const*>(itemBytes)));
  }

  Item& item(std::size_t i) {
    FOLLY_SAFE_DCHECK(this->occupied(i), "");
    compiler_may_unsafely_assume(this != getSomeEmptyInstance());
    return *std::launder(itemAddr(i));
  }

  Item const& citem(std::size_t i) const {
    FOLLY_SAFE_DCHECK(this->occupied(i), "");
    return *std::launder(itemAddr(i));
  }

  static F14Chunk& owner(Item& item, std::size_t index) {
    auto rawAddr =
        static_cast<uint8_t*>(static_cast<void*>(std::addressof(item))) -
        kItemsOffset - index * sizeof(Item);
    auto chunkAddr = static_cast<F14Chunk*>(static_cast<void*>(rawAddr));
    FOLLY_SAFE_DCHECK(std::addressof(item) == chunkAddr->itemAddr(index), "");
    return *chunkAddr;
  }
};

////////////////

// PackedChunkItemPtr points to an Item in an F14Chunk, allowing both the
// Item& and its index to be recovered.  It sorts by the address of the
// item, and it only works for items that are in a properly-aligned chunk.

// generic form, not actually packed
template <typename Ptr>
class PackedChunkItemPtr {
 public:
  PackedChunkItemPtr() = default;

  PackedChunkItemPtr(Ptr p, std::size_t i) noexcept : ptr_{p}, index_{i} {
    FOLLY_SAFE_DCHECK(ptr_ != nullptr || index_ == 0, "");
  }

  Ptr ptr() const { return ptr_; }

  std::size_t index() const { return index_; }

  bool operator<(PackedChunkItemPtr const& rhs) const {
    FOLLY_SAFE_DCHECK(ptr_ != rhs.ptr_ || index_ == rhs.index_, "");
    return ptr_ < rhs.ptr_;
  }

  bool operator==(PackedChunkItemPtr const& rhs) const {
    FOLLY_SAFE_DCHECK(ptr_ != rhs.ptr_ || index_ == rhs.index_, "");
    return ptr_ == rhs.ptr_;
  }

  bool operator!=(PackedChunkItemPtr const& rhs) const {
    return !(*this == rhs);
  }

 private:
  Ptr ptr_{};
  std::size_t index_{};
};

// Bare pointer form, packed into a uintptr_t.  Uses only bits wasted by
// alignment, so it works on 32-bit and 64-bit platforms
template <typename T>
class PackedChunkItemPtr<T*> {
  static_assert((alignof(F14Chunk<T>) % 16) == 0);

  // Chunks are 16-byte aligned, so we can maintain a packed pointer to a
  // chunk item by packing the 4-bit item index into the least significant
  // bits of a pointer to the chunk itself.  This makes ItemIter::pack
  // more expensive, however, since it has to compute the chunk address.
  //
  // Chunk items have varying alignment constraints, so it would seem
  // to be that we can't do a similar trick while using only bit masking
  // operations on the Item* itself.  It happens to be, however, that if
  // sizeof(Item) is not a multiple of 16 then we can recover a portion
  // of the index bits from the knowledge that the Item-s are stored in
  // an array that is itself 16-byte aligned.
  //
  // If kAlignBits is the number of trailing zero bits in sizeof(Item)
  // (up to 4), then we can borrow those bits to store kAlignBits of the
  // index directly.  We can recover (4 - kAlignBits) bits of the index
  // from the item pointer itself, by defining/observing that
  //
  // A = kAlignBits                  (A <= 4)
  //
  // S = (sizeof(Item) % 16) >> A    (shifted-away bits are all zero)
  //
  // R = (itemPtr % 16) >> A         (shifted-away bits are all zero)
  //
  // M = 16 >> A
  //
  // itemPtr % 16   = (index * sizeof(Item)) % 16
  //
  // (R * 2^A) % 16 = (index * (sizeof(Item) % 16)) % 16
  //
  // (R * 2^A) % 16 = (index * 2^A * S) % 16
  //
  // R % M          = (index * S) % M
  //
  // S is relatively prime with M, so a multiplicative inverse is easy
  // to compute
  //
  // Sinv = S^(M - 1) % M
  //
  // (R * Sinv) % M = index % M
  //
  // This lets us recover the bottom bits of the index.  When sizeof(T)
  // is 8-byte aligned kSizeInverse will always be 1.  When sizeof(T)
  // is 4-byte aligned kSizeInverse will be either 1 or 3.

  // returns pow(x, y) % m
  static constexpr uintptr_t powerMod(uintptr_t x, uintptr_t y, uintptr_t m) {
    return y == 0 ? 1 : (x * powerMod(x, y - 1, m)) % m;
  }

  static constexpr uintptr_t kIndexBits = 4;
  static constexpr uintptr_t kIndexMask = (uintptr_t{1} << kIndexBits) - 1;

  static constexpr uintptr_t kAlignBits = constexpr_min(
      uintptr_t{4}, constexpr_find_first_set(uintptr_t{sizeof(T)}) - 1);

  static constexpr uintptr_t kAlignMask = (uintptr_t{1} << kAlignBits) - 1;

  static constexpr uintptr_t kModulus = uintptr_t{1}
      << (kIndexBits - kAlignBits);
  static constexpr uintptr_t kSizeInverse =
      powerMod(sizeof(T) >> kAlignBits, kModulus - 1, kModulus);

 public:
  PackedChunkItemPtr() = default;

  PackedChunkItemPtr(T* p, std::size_t i) noexcept {
    uintptr_t encoded = i >> (kIndexBits - kAlignBits);
    assume((encoded & ~kAlignMask) == 0);
    raw_ = reinterpret_cast<uintptr_t>(p) | encoded;
    FOLLY_SAFE_DCHECK(p == ptr(), "");
    FOLLY_SAFE_DCHECK(i == index(), "");
  }

  T* ptr() const { return reinterpret_cast<T*>(raw_ & ~kAlignMask); }

  std::size_t index() const {
    auto encoded = (raw_ & kAlignMask) << (kIndexBits - kAlignBits);
    auto deduced =
        ((raw_ >> kAlignBits) * kSizeInverse) & (kIndexMask >> kAlignBits);
    return encoded | deduced;
  }

  bool operator<(PackedChunkItemPtr const& rhs) const {
    return raw_ < rhs.raw_;
  }
  bool operator==(PackedChunkItemPtr const& rhs) const {
    return raw_ == rhs.raw_;
  }
  bool operator!=(PackedChunkItemPtr const& rhs) const {
    return !(*this == rhs);
  }

 private:
  uintptr_t raw_{};
};

template <typename ChunkPtr>
class F14ItemIter {
 private:
  using Chunk = typename std::pointer_traits<ChunkPtr>::element_type;

 public:
  using Item = typename Chunk::Item;
  using ItemPtr = typename std::pointer_traits<ChunkPtr>::template rebind<Item>;
  using ItemConstPtr =
      typename std::pointer_traits<ChunkPtr>::template rebind<Item const>;

  using Packed = PackedChunkItemPtr<ItemPtr>;

  //// PUBLIC

  F14ItemIter() noexcept : itemPtr_{nullptr}, index_{0} {}

  // default copy and move constructors and assignment operators are correct

  explicit F14ItemIter(Packed const& packed)
      : itemPtr_{packed.ptr()}, index_{packed.index()} {}

  F14ItemIter(ChunkPtr chunk, std::size_t index)
      : itemPtr_{std::pointer_traits<ItemPtr>::pointer_to(chunk->item(index))},
        index_{index} {
    FOLLY_SAFE_DCHECK(index < Chunk::kCapacity, "");
    assume(
        std::pointer_traits<ItemPtr>::pointer_to(chunk->item(index)) !=
        nullptr);
    assume(itemPtr_ != nullptr);
  }

  FOLLY_ALWAYS_INLINE void advanceImpl(bool checkEof, bool likelyDead) {
    auto c = chunk();

    // common case is packed entries
    while (index_ > 0) {
      --index_;
      --itemPtr_;
      if (c->occupied(index_)) [[likely]] {
        return;
      }
    }

    // It's fairly common for an iterator to be advanced and then become
    // dead, for example in the return value from erase(iter) or in
    // the last step of a loop.  We'd like to make sure that the entire
    // advance() method can be eliminated by the compiler's dead code
    // elimination pass.  To do that it must eliminate the loops, which
    // requires it to prove that they have no side effects.  It's easy
    // to show that there are no escaping stores, but at the moment
    // compilers also consider an infinite loop to be a side effect.
    // (There are parts of the standard that would allow them to treat
    // this as undefined behavior, but at the moment they don't exploit
    // those clauses.)
    //
    // The following loop should really be a while loop, which would
    // save a register, some instructions, and a conditional branch,
    // but by writing it as a for loop the compiler can prove to itself
    // that it will eventually terminate.  (No matter that even if the
    // loop executed in a single cycle it would take about 200 years to
    // run all 2^64 iterations.)
    //
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=82776 has the bug we
    // filed about the issue.  while (true) {
    for (std::size_t i = 1; !likelyDead || i != 0; ++i) {
      if (checkEof) {
        // exhausted the current chunk
        if (c->eof()) [[unlikely]] {
          FOLLY_SAFE_DCHECK(index_ == 0, "");
          itemPtr_ = nullptr;
          return;
        }
      } else {
        FOLLY_SAFE_DCHECK(!c->eof(), "");
      }
      c = std::pointer_traits<ChunkPtr>::pointer_to(*Chunk::prevChunkRaw(&*c));
      auto last = c->lastOccupied();
      if (checkEof && !likelyDead) {
        prefetchAddr(Chunk::prevChunkRaw(&*c));
      }
      if (last.hasIndex()) [[likely]] {
        index_ = last.index();
        itemPtr_ = std::pointer_traits<ItemPtr>::pointer_to(c->item(index_));
        return;
      }
    }
  }

  void precheckedAdvance() { advanceImpl(false, false); }

  FOLLY_ALWAYS_INLINE void advance() { advanceImpl(true, false); }

  FOLLY_ALWAYS_INLINE void advanceLikelyDead() { advanceImpl(true, true); }

  ChunkPtr chunk() const {
    return std::pointer_traits<ChunkPtr>::pointer_to(
        Chunk::owner(*itemPtr_, index_));
  }

  std::size_t index() const { return index_; }

  Item* itemAddr() const { return std::addressof(*itemPtr_); }
  Item& item() const { return *itemPtr_; }
  Item const& citem() const { return *itemPtr_; }

  bool atEnd() const { return itemPtr_ == nullptr; }

  Packed pack() const { return Packed{itemPtr_, static_cast<uint8_t>(index_)}; }

  bool operator==(F14ItemIter const& rhs) const {
    // this form makes iter == end() into a single null check after inlining
    // and constant propagation
    return itemPtr_ == rhs.itemPtr_;
  }

  bool operator!=(F14ItemIter const& rhs) const { return !(*this == rhs); }

 private:
  ItemPtr itemPtr_;
  std::size_t index_;
};

////////////////

struct PackedSizeAndChunkShift {
 private:
  // F14Table's chunk count is a power of 2 that is at least 1 and at
  // most 1 << 63. In order to encode it efficiently, we store its base 2
  // logarithm `n`, so that (1UL << n) is the chunkCount. Despite
  // needing only 6 bits to encode the shift, we reserve 8 bits to
  // avoid extra masking, leaving 56 bits for the size, which is
  // plenty.

  // We use the least significant bits of
  // packedSizeAndChunkShift_ for the shift so that size access
  // is just a single shift and shift access is a normal 8-bit load.
  static constexpr uint32_t kSizeShift = 8;
  static constexpr uint32_t kChunkCountShiftMask = (1 << kSizeShift) - 1;
  uint64_t packedSizeAndChunkShift_{0};

 public:
  // subtract 1 because we can't represent 1 << 64, triggering UBSAN.
  static constexpr uint8_t kMaxSupportedChunkShift =
      std::numeric_limits<uint64_t>::digits - 1;

  static constexpr std::size_t kMaxSize =
      (size_t(1) << (std::numeric_limits<std::size_t>::digits - kSizeShift)) -
      1;

  uint64_t size() const noexcept {
    return packedSizeAndChunkShift_ >> kSizeShift;
  }

  uint8_t chunkShift() const noexcept {
    return packedSizeAndChunkShift_ & kChunkCountShiftMask;
  }

  std::size_t chunkCount() const noexcept {
    const auto chunkCountShift = chunkShift();
    return std::size_t(1) << chunkCountShift;
  }

  void setSize(std::size_t sz) noexcept {
    packedSizeAndChunkShift_ =
        chunkShift() | (static_cast<uint64_t>(sz) << kSizeShift);
  }

  void setChunkCount(std::size_t newCount) {
    FOLLY_SAFE_DCHECK(
        folly::isPowTwo(newCount), newCount); // note that this forbids 0!
    const auto shift = findFirstSet(newCount) - 1; // firstSet is 1-based.
    packedSizeAndChunkShift_ =
        (static_cast<uint64_t>(size()) << kSizeShift) | shift;
    FOLLY_SAFE_DCHECK(chunkCount() == newCount, "");
  }

  void swap(PackedSizeAndChunkShift& rhs) noexcept {
    std::swap(packedSizeAndChunkShift_, rhs.packedSizeAndChunkShift_);
  }
};

struct UnpackedSizeAndChunkShift {
 private:
  // Simple implementation for 32-bit systems: just use two words.
  std::size_t size_ = 0;
  uint8_t chunkShift_ = 0;

 public:
  // subtract 1 because we can't represent 1 << 32, triggering UBSAN.
  static constexpr uint8_t kMaxSupportedChunkShift =
      std::numeric_limits<std::size_t>::digits - 1;

  static constexpr std::size_t kMaxSize =
      std::numeric_limits<std::size_t>::max();

  uint64_t size() const noexcept { return size_; }

  uint8_t chunkShift() const noexcept { return chunkShift_; }

  std::size_t chunkCount() const noexcept {
    const auto chunkCountShift = chunkShift();
    return std::size_t(1) << chunkCountShift;
  }

  void setSize(std::size_t sz) noexcept { size_ = sz; }

  void setChunkCount(std::size_t newCount) {
    FOLLY_SAFE_DCHECK(
        folly::isPowTwo(newCount), newCount); // note that this forbids 0!
    const auto shift = findFirstSet(newCount) - 1; // firstSet is 1-based.
    FOLLY_SAFE_DCHECK(shift <= std::numeric_limits<uint8_t>::max(), "");
    chunkShift_ = static_cast<uint8_t>(shift);
    FOLLY_SAFE_DCHECK(chunkCount() == newCount, "");
  }

  void swap(UnpackedSizeAndChunkShift& rhs) noexcept {
    std::swap(size_, rhs.size_);
    std::swap(chunkShift_, rhs.chunkShift_);
  }
};

using SizeAndChunkShift = std::conditional_t<
    sizeof(uint64_t) == sizeof(std::size_t),
    PackedSizeAndChunkShift,
    UnpackedSizeAndChunkShift>;

template <typename ItemIter, bool EnablePackedItemIter>
struct SizeAndChunkShiftAndPackedBegin {
 private:
  SizeAndChunkShift sizeAndChunkShift_;

  typename ItemIter::Packed packedBegin_{};

 public:
  uint64_t size() const { return sizeAndChunkShift_.size(); }

  uint8_t chunkShift() const { return sizeAndChunkShift_.chunkShift(); }

  std::size_t chunkCount() const { return sizeAndChunkShift_.chunkCount(); }

  void setSize(uint64_t sz) { sizeAndChunkShift_.setSize(sz); }

  void incrementSize() { sizeAndChunkShift_.setSize(size() + 1); }

  void decrementSize() { sizeAndChunkShift_.setSize(size() - 1); }

  void setChunkCount(std::size_t count) {
    sizeAndChunkShift_.setChunkCount(count);
  }

  typename ItemIter::Packed& packedBegin() { return packedBegin_; }

  typename ItemIter::Packed const& packedBegin() const { return packedBegin_; }

  void swap(SizeAndChunkShiftAndPackedBegin& rhs) noexcept {
    sizeAndChunkShift_.swap(rhs.sizeAndChunkShift_);
    std::swap(packedBegin_, rhs.packedBegin_);
  }
};

template <typename ItemIter>
struct SizeAndChunkShiftAndPackedBegin<ItemIter, false> {
 private:
  SizeAndChunkShift sizeAndChunkShift_;

 public:
  uint64_t size() const { return sizeAndChunkShift_.size(); }

  uint8_t chunkShift() const { return sizeAndChunkShift_.chunkShift(); }

  std::size_t chunkCount() const { return sizeAndChunkShift_.chunkCount(); }

  void setSize(uint64_t sz) { sizeAndChunkShift_.setSize(sz); }

  void incrementSize() { sizeAndChunkShift_.setSize(size() + 1); }

  void decrementSize() { sizeAndChunkShift_.setSize(size() - 1); }

  void setChunkCount(std::size_t count) {
    sizeAndChunkShift_.setChunkCount(count);
  }

  [[noreturn]] typename ItemIter::Packed& packedBegin() {
    assume_unreachable();
  }

  [[noreturn]] typename ItemIter::Packed const& packedBegin() const {
    assume_unreachable();
  }
};

template <typename Policy>
class F14Table : public Policy {
 public:
  using Item = typename Policy::Item;

  using value_type = typename Policy::Value;
  using allocator_type = typename Policy::Alloc;

 private:
  using Alloc = typename Policy::Alloc;
  using AllocTraits = typename Policy::AllocTraits;
  using Hasher = typename Policy::Hasher;
  using InternalSizeType = typename Policy::InternalSizeType;
  using KeyEqual = typename Policy::KeyEqual;

  using Policy::kAllocIsAlwaysEqual;
  using Policy::kContinuousCapacity;
  using Policy::kDefaultConstructIsNoexcept;
  using Policy::kEnableItemIteration;
  using Policy::kSwapIsNoexcept;

  using Policy::destroyItemOnClear;
  using Policy::isAvalanchingHasher;
  using Policy::prefetchBeforeCopy;
  using Policy::prefetchBeforeDestroy;
  using Policy::prefetchBeforeRehash;
  using Policy::shouldAssume32BitHash;

  using ByteAlloc = typename AllocTraits::template rebind_alloc<uint8_t>;
  using BytePtr = typename std::allocator_traits<ByteAlloc>::pointer;

  using Chunk = F14Chunk<Item>;
  using ChunkPtr =
      typename std::pointer_traits<BytePtr>::template rebind<Chunk>;
  using ChunkHeaderPtr =
      typename std::pointer_traits<BytePtr>::template rebind<F14ChunkHeader>;

  using HashPair = typename F14HashToken::HashPair;

 public:
  using ItemIter = F14ItemIter<ChunkPtr>;

 private:
  //////// begin fields

  ChunkHeaderPtr chunks_{Chunk::getSomeEmptyInstance()};
  SizeAndChunkShiftAndPackedBegin<ItemIter, kEnableItemIteration>
      sizeAndChunkShiftAndPackedBegin_;

  //////// end fields

  ChunkPtr chunks() const {
    return std::pointer_traits<ChunkPtr>::pointer_to(*chunksToAddress());
  }

  Chunk* chunksToAddress() const {
    return static_cast<Chunk*>(std::to_address(chunks_));
  }

  uint8_t chunkShift() const {
    return sizeAndChunkShiftAndPackedBegin_.chunkShift();
  }

  std::size_t chunkCount() const {
    return sizeAndChunkShiftAndPackedBegin_.chunkCount();
  }

  std::size_t moduloByChunkCount(std::size_t index) const {
#ifdef __BMI2__
    // TODO: remove once the compiler selects BZHI on its own.
    // see codegen for: codeSize_find_F14value, defined in:
    // folly/container/test/F14SmallOverheads.cpp
    return _bzhi_u64(index, chunkShift());
#else
    return index & ((std::size_t(1) << chunkShift()) - 1);
#endif
  }

  ChunkPtr chunkAt(std::size_t i) const {
    return std::pointer_traits<ChunkPtr>::pointer_to(
        *Chunk::chunkRawAt(chunksToAddress(), i));
  }

  std::size_t chunkIndex(Chunk const* chunk) const {
    return Chunk::chunkRawIndex(chunksToAddress(), chunk);
  }

  void swapContents(F14Table& rhs) noexcept {
    using std::swap;
    swap(chunks_, rhs.chunks_);
    swap(
        sizeAndChunkShiftAndPackedBegin_, rhs.sizeAndChunkShiftAndPackedBegin_);
  }

 public:
  // Equivalent to F14Table(0, ...), but implemented separately to avoid forcing
  // a reserve() instantiation in the common case.
  constexpr F14Table() noexcept(Policy::kDefaultConstructIsNoexcept)
      : Policy{Hasher{}, KeyEqual{}, Alloc{}} {}

  F14Table(
      std::size_t initialCapacity,
      Hasher const& hasher,
      KeyEqual const& keyEqual,
      Alloc const& alloc)
      : Policy{hasher, keyEqual, alloc} {
    debugModeOnReserve(initialCapacity);
    initialReserve(initialCapacity);
  }

  F14Table(F14Table const& rhs) : Policy{rhs} { buildFromF14Table(rhs); }

  F14Table(F14Table const& rhs, Alloc const& alloc) : Policy{rhs, alloc} {
    buildFromF14Table(rhs);
  }

  F14Table(F14Table&& rhs) noexcept(
      std::is_nothrow_move_constructible<Hasher>::value &&
      std::is_nothrow_move_constructible<KeyEqual>::value &&
      std::is_nothrow_move_constructible<Alloc>::value)
      : Policy{std::move(rhs)} {
    swapContents(rhs);
  }

  F14Table(F14Table&& rhs, Alloc const& alloc) noexcept(kAllocIsAlwaysEqual)
      : Policy{std::move(rhs), alloc} {
    // if-constexpr allows avoiding dependence on usable Hasher etc.
    if constexpr (kAllocIsAlwaysEqual) {
      // move storage (common case)
      swapContents(rhs);
    } else if (this->alloc() == rhs.alloc()) {
      // move storage (common case)
      swapContents(rhs);
    } else {
      // new storage because allocators unequal, move values (rare case)
      buildFromF14Table(std::move(rhs));
    }
  }

  F14Table& operator=(F14Table const& rhs) {
    if (this != &rhs) {
      reset();
      static_cast<Policy&>(*this) = rhs;
      buildFromF14Table(rhs);
    }
    return *this;
  }

  F14Table& operator=(F14Table&& rhs) noexcept(
      std::is_nothrow_move_assignable<Hasher>::value &&
      std::is_nothrow_move_assignable<KeyEqual>::value &&
      (kAllocIsAlwaysEqual ||
       (AllocTraits::propagate_on_container_move_assignment::value &&
        std::is_nothrow_move_assignable<Alloc>::value))) {
    if (this != &rhs) {
      reset();
      static_cast<Policy&>(*this) = std::move(rhs);
      // if-constexpr allows avoiding dependence on usable Hasher etc.
      if constexpr (
          AllocTraits::propagate_on_container_move_assignment::value ||
          kAllocIsAlwaysEqual) {
        // move storage (common case)
        swapContents(rhs);
      } else if (this->alloc() == rhs.alloc()) {
        // move storage (common case)
        swapContents(rhs);
      } else {
        // new storage because allocators unequal, move values (rare case)
        buildFromF14Table(std::move(rhs));
      }
    }
    return *this;
  }

  ~F14Table() { reset(); }

  void swap(F14Table& rhs) noexcept(kSwapIsNoexcept) {
    // If propagate_on_container_swap is false and allocators are
    // not equal, the only way to accomplish a swap would be to do
    // dynamic allocation and then move (or swap) each contained value.
    // AllocatorAwareContainer-s are not supposed to attempt this, but
    // rather are supposed to have undefined behavior in that case.
    FOLLY_SAFE_CHECK(
        AllocTraits::propagate_on_container_swap::value ||
            kAllocIsAlwaysEqual || this->alloc() == rhs.alloc(),
        "swap is undefined for unequal non-propagating allocators");
    this->swapPolicy(rhs);
    swapContents(rhs);
  }

 private:
  static HashPair splitHash(std::size_t hash) {
    return f14::detail::splitHashImpl<Hasher, value_type>(hash);
  }
  //////// memory management helpers

  static std::size_t computeCapacity(
      std::size_t chunkCount, std::size_t scale) {
    FOLLY_SAFE_DCHECK(!(chunkCount > 1 && scale == 0), "");
    FOLLY_SAFE_DCHECK(
        scale < (std::size_t{1} << Chunk::kCapacityScaleBits), "");
    FOLLY_SAFE_DCHECK((chunkCount & (chunkCount - 1)) == 0, "");
    return (((chunkCount - 1) >> Chunk::kCapacityScaleShift) + 1) * scale;
  }

  std::pair<std::size_t, std::size_t> computeChunkCountAndScale(
      std::size_t desiredCapacity,
      bool continuousSingleChunkCapacity,
      bool continuousMultiChunkCapacity) const {
    if (desiredCapacity <= Chunk::kCapacity) {
      // we can go to 100% capacity in a single chunk with no problem
      if (!continuousSingleChunkCapacity) {
        if (desiredCapacity <= 2) {
          desiredCapacity = 2;
        } else if (desiredCapacity <= 6) {
          desiredCapacity = 6;
        } else {
          desiredCapacity = Chunk::kCapacity;
        }
      }
      auto rv = std::make_pair(std::size_t{1}, desiredCapacity);
      FOLLY_SAFE_DCHECK(
          computeCapacity(rv.first, rv.second) == desiredCapacity, "");
      return rv;
    } else {
      std::size_t minChunks =
          (desiredCapacity - 1) / Chunk::kDesiredCapacity + 1;
      std::size_t chunkPow = findLastSet(minChunks - 1);
      if (chunkPow == 8 * sizeof(std::size_t)) {
        throw_exception<std::bad_alloc>();
      }

      std::size_t chunkCount = std::size_t{1} << chunkPow;

      // Let cc * scale be the actual capacity.
      // cc = ((chunkCount - 1) >> kCapacityScaleShift) + 1.
      // If chunkPow >= kCapacityScaleShift, then cc = chunkCount >>
      // kCapacityScaleShift = 1 << (chunkPow - kCapacityScaleShift),
      // otherwise it equals 1 = 1 << 0.  Let cc = 1 << ss.
      std::size_t ss = chunkPow >= Chunk::kCapacityScaleShift
          ? chunkPow - Chunk::kCapacityScaleShift
          : 0;

      std::size_t scale;
      if (continuousMultiChunkCapacity) {
        // (1 << ss) * scale >= desiredCapacity
        scale = ((desiredCapacity - 1) >> ss) + 1;
      } else {
        // (1 << ss) * scale == chunkCount * kDesiredCapacity
        scale = Chunk::kDesiredCapacity << (chunkPow - ss);
      }

      std::size_t actualCapacity = computeCapacity(chunkCount, scale);
      FOLLY_SAFE_DCHECK(actualCapacity >= desiredCapacity, "");
      if (actualCapacity > max_size()) {
        throw_exception<std::bad_alloc>();
      }

      return std::make_pair(chunkCount, scale);
    }
  }

  static std::size_t chunkAllocSize(
      std::size_t chunkCount, std::size_t capacityScale) {
    FOLLY_SAFE_DCHECK(chunkCount > 0, "");
    FOLLY_SAFE_DCHECK(!(chunkCount > 1 && capacityScale == 0), "");
    if (chunkCount == 1) {
      static_assert(sizeof(Chunk) == Chunk::kItemsOffset);
      return Chunk::kItemsOffset +
          sizeof(Item) * computeCapacity(1, capacityScale);
    } else {
      return Chunk::kChunkStride * chunkCount;
    }
  }

  ChunkHeaderPtr initializeChunks(
      BytePtr raw, std::size_t chunkCount, std::size_t capacityScale) {
    static_assert(std::is_trivial<Chunk>::value, "F14Chunk should be POD");
    auto chunks = static_cast<Chunk*>(static_cast<void*>(&*raw));
    for (std::size_t i = 0; i < chunkCount; ++i) {
      Chunk::chunkRawAt(chunks, i)->clear();
    }
    chunks->markEof(capacityScale);
    return std::pointer_traits<ChunkHeaderPtr>::pointer_to(
        static_cast<F14ChunkHeader&>(*chunks));
  }

  std::size_t itemCount() const noexcept {
    if (chunkShift() == 0) {
      return computeCapacity(1, Chunk::capacityScale(chunksToAddress()));
    } else {
      return chunkCount() * Chunk::kCapacity;
    }
  }

 public:
  ItemIter begin() const noexcept {
    FOLLY_SAFE_DCHECK(kEnableItemIteration, "");
    return ItemIter{sizeAndChunkShiftAndPackedBegin_.packedBegin()};
  }

  ItemIter end() const noexcept { return ItemIter{}; }

  bool empty() const noexcept { return size() == 0; }

  std::size_t size() const noexcept {
    return to_narrow(sizeAndChunkShiftAndPackedBegin_.size());
  }

  std::size_t max_size() const noexcept {
    auto& a = this->alloc();
    return std::min<std::size_t>(
        {SizeAndChunkShift::kMaxSize,
         std::numeric_limits<InternalSizeType>::max(),
         AllocTraits::max_size(a)});
  }

  std::size_t bucket_count() const noexcept {
    return computeCapacity(
        chunkCount(), Chunk::capacityScale(chunksToAddress()));
  }

  std::size_t max_bucket_count() const noexcept { return max_size(); }

  float load_factor() const noexcept {
    return empty()
        ? 0.0f
        : static_cast<float>(size()) / static_cast<float>(bucket_count());
  }

  float max_load_factor() const noexcept { return 1.0f; }

  void max_load_factor(float) noexcept {
    // Probing hash tables can't run load factors >= 1 (unlike chaining
    // tables).  In addition, we have measured that there is little or
    // no performance advantage to running a smaller load factor (cache
    // locality losses outweigh the small reduction in probe lengths,
    // often making it slower).  Therefore, we've decided to just fix
    // max_load_factor at 1.0f regardless of what the user requests.
    // This has an additional advantage that we don't have to store it.
    // Taking alignment into consideration this makes every F14 table
    // 8 bytes smaller, and is part of the reason an empty F14NodeMap
    // is almost half the size of an empty std::unordered_map (32 vs
    // 56 bytes).
    //
    // I don't have a strong opinion on whether we should remove this
    // method or leave a stub, let ngbronson or xshi know if you have a
    // compelling argument either way.
  }

 private:
  // Our probe strategy is to advance through additional chunks with
  // a stride that is key-specific.  This is called double hashing,
  // and is a well known and high quality probing strategy.  So long as
  // the stride and the chunk count are relatively prime, we will visit
  // every chunk once and then return to the original chunk, letting us
  // detect and end the cycle.  The chunk count is a power of two, so
  // we can satisfy the relatively prime part by choosing an odd stride.
  // We've already computed a high quality secondary hash value for the
  // tag, so we just use it for the second probe hash as well.
  //
  // At the maximum load factor of 12/14, expected probe length for a
  // find hit is 1.041, with 99% of keys found in the first three chunks.
  // Expected probe length for a find miss (or insert) is 1.275, with a
  // p99 probe length of 4 (fewer than 1% of failing find look at 5 or
  // more chunks).
  //
  // This code is structured so you can try various ways of encoding
  // the current probe state.  For example, at the moment the probe's
  // state is the position in the cycle and the resulting chunk index is
  // computed from that inside probeCurrentIndex.  We could also make the
  // probe state the chunk index, and then increment it by hp.second *
  // 2 + 1 in probeAdvance.  Wrapping can be applied early or late as
  // well.  This particular code seems to be easier for the optimizer
  // to understand.
  //
  // We could also implement probing strategies that resulted in the same
  // tour for every key initially assigned to a chunk (linear probing or
  // quadratic), but that results in longer probe lengths.  In particular,
  // the cache locality wins of linear probing are not worth the increase
  // in probe lengths (extra work and less branch predictability) in
  // our experiments.

  std::size_t probeDelta(HashPair hp) const { return 2 * hp.second + 1; }

  // TRICKY!  It may seem strange to have a std::size_t needle and narrow
  // it at the last moment, rather than making HashPair::second be a
  // uint8_t, but the latter choice sometimes leads to a performance
  // problem.
  //
  // On architectures with SSE2 but not AVX2, _mm_set1_epi8 expands
  // to multiple instructions.  One of those is a MOVD of either 4 or
  // 8 byte width.  Only the bottom byte of that move actually affects
  // the result, but if a 1-byte needle has been spilled then this will
  // be a 4 byte load.  GCC 5.5 has been observed to reload needle
  // (or perhaps fuse a reload and part of a previous static_cast)
  // needle using a MOVZX with a 1 byte load in parallel with the MOVD.
  // This combination causes a failure of store-to-load forwarding,
  // which has a big performance penalty (60 nanoseconds per find on
  // a microbenchmark).  Keeping needle >= 4 bytes avoids the problem
  // and also happens to result in slightly more compact assembly.

  FOLLY_ALWAYS_INLINE auto loadNeedleV(std::size_t needle) const {
#if FOLLY_NEON
    return vdupq_n_u8(static_cast<uint8_t>(needle));
#elif FOLLY_SSE >= 2
    return _mm_set1_epi8(static_cast<uint8_t>(needle));
#else
    return needle;
#endif
  }

  enum class Prefetch { DISABLED, ENABLED };

  template <typename K>
  FOLLY_ALWAYS_INLINE ItemIter
  findImpl(HashPair hp, K const& key, Prefetch prefetch) const {
    FOLLY_SAFE_DCHECK(hp.second >= 1 && hp.second < 0x100, "");
    std::size_t index = hp.first;
    std::size_t step = probeDelta(hp);
    auto needleV = loadNeedleV(hp.second);
    for (std::size_t tries = chunkCount(); tries > 0;) {
      ChunkPtr chunk = chunkAt(moduloByChunkCount(index));
      if (prefetch == Prefetch::ENABLED && Chunk::kChunkStride > 64) {
        prefetchAddr(chunk->itemAddr(8));
      }
      ItemIter found{};
      if (chunk->forEachTagMatch(needleV, [&](unsigned i) {
            if (this->keyMatchesItem(key, chunk->item(i))) [[likely]] {
              // Tag match and key match were both successful.  The chance
              // of a false tag match is ~1/255 for each key in the chunk
              // (with a proper hash function).
              found = ItemIter{chunk, i};
              return true;
            }
            return false;
          })) {
        return found;
      }
      if (chunk->outboundOverflowCount() == 0) [[likely]] {
        // No keys that wanted to be placed in this chunk were denied
        // entry, so our search is over.  This is the common case.
        break;
      }
      --tries;
      index += step;
    }
    // Loop exit because tries is exhausted is rare, but possible.
    // That means that for every chunk there is currently a key present
    // in the map that visited that chunk on its probe search but ended
    // up somewhere else, and we have searched every chunk.
    return ItemIter{};
  }

  template <typename K>
  HashPair computeHash(K const& key) const {
    return splitHash(this->computeKeyHash(key));
  }

  template <typename HKKey, typename HKHasher, typename HKEqual>
  HashPair computeHash(
      F14HashedKey<HKKey, HKHasher, HKEqual> const& hashedKey) const {
    static_assert(HeterogeneousPreHashCompatible<Hasher, HKHasher>::value);
    static_assert(HeterogeneousPreHashCompatible<HKEqual, KeyEqual>::value);
    return static_cast<HashPair>(hashedKey.getHashToken());
  }

 public:
  // prehash()/prefetch() split the work of find(key) into three calls, enabling
  // you to manually implement loop pipelining for hot bulk lookups. prehash()
  // computes the hash and prefetch() prefetches the first computed memory
  // location, and the two-arg find(F14HashToken, K) performs the rest of the
  // search.
  template <typename K>
  F14HashToken prehash(K const& key) const {
    return F14HashToken{computeHash(key)};
  }

  template <typename K>
  F14HashToken prehash(K const& key, std::size_t hash) const {
    FOLLY_SAFE_DCHECK(hash == this->computeKeyHash(key));
    return F14HashToken{splitHash(hash)};
  }

  void prefetch(F14HashToken const& token) const {
    FOLLY_SAFE_DCHECK(chunks_ != nullptr, "");
    ChunkPtr firstChunk = chunkAt(moduloByChunkCount(token.hp_.first));
    prefetchAddr(firstChunk);
  }

  template <typename K>
  FOLLY_ALWAYS_INLINE ItemIter find(K const& key) const {
    const auto sz = size();
    if (sz <= 1) {
      if (sz == 0) {
        return ItemIter{};
      }
      // There is no easy way to obtain a begin iterator with the current policy
      // design.
      // As a result, F14Vector* containers (kEnableItemIteration == false) do
      // not benefit from this optimization yet.
      if constexpr (kEnableItemIteration) {
        ItemIter beg = begin();
        return this->keyMatchesItem(key, beg.citem()) ? beg : ItemIter{};
      }
    }
    auto hp = computeHash(key);
    return findImpl(hp, key, Prefetch::ENABLED);
  }

  template <typename K>
  FOLLY_ALWAYS_INLINE ItemIter
  find(F14HashToken const& token, K const& key) const {
    FOLLY_SAFE_DCHECK(computeHash(key) == static_cast<HashPair>(token), "");
    return findImpl(static_cast<HashPair>(token), key, Prefetch::DISABLED);
  }

  // Searches for a key using a key predicate that is a refinement
  // of key equality.  func(k) should return true only if k is equal
  // to key according to key_eq(), but is allowed to apply additional
  // constraints.
  template <typename K, typename F>
  FOLLY_ALWAYS_INLINE ItemIter findMatching(K const& key, F&& func) const {
    auto hp = computeHash(key);
    std::size_t index = hp.first;
    auto needleV = loadNeedleV(hp.second);
    std::size_t step = probeDelta(hp);
    for (std::size_t tries = chunkCount(); tries > 0; --tries) {
      ChunkPtr chunk = chunkAt(moduloByChunkCount(index));
      if (Chunk::kChunkStride > 64) {
        prefetchAddr(chunk->itemAddr(8));
      }
      ItemIter found{};
      if (chunk->forEachTagMatch(needleV, [&](unsigned i) {
            if (func(this->keyForValue(this->valueAtItem(chunk->item(i)))))
                [[likely]] {
              found = ItemIter{chunk, i};
              return true;
            }
            return false;
          })) {
        return found;
      }
      if (chunk->outboundOverflowCount() == 0) [[likely]] {
        break;
      }
      index += step;
    }
    return ItemIter{};
  }

 private:
  void adjustSizeAndBeginAfterInsert(ItemIter iter) {
    if constexpr (kEnableItemIteration) {
      // packedBegin is the max of all valid ItemIter::pack()
      auto packed = iter.pack();
      if (sizeAndChunkShiftAndPackedBegin_.packedBegin() < packed) {
        sizeAndChunkShiftAndPackedBegin_.packedBegin() = packed;
      }
    }

    sizeAndChunkShiftAndPackedBegin_.incrementSize();
  }

  // Ignores hp if pos.chunk()->hostedOverflowCount() == 0
  void eraseBlank(ItemIter iter, HashPair hp) {
    iter.chunk()->clearTag(iter.index());

    if (iter.chunk()->hostedOverflowCount() != 0) {
      // clean up
      std::size_t index = hp.first;
      std::size_t delta = probeDelta(hp);
      uint8_t hostedOp = 0;
      while (true) {
        ChunkPtr chunk = chunkAt(moduloByChunkCount(index));
        if (chunk == iter.chunk()) {
          chunk->adjustHostedOverflowCount(hostedOp);
          break;
        }
        chunk->decrOutboundOverflowCount();
        hostedOp = Chunk::kDecrHostedOverflowCount;
        index += delta;
      }
    }
  }

  [[FOLLY_ATTR_GNU_COLD]] FOLLY_NOINLINE static void eraseBlankCold(
      F14Table* self, ItemIter iter, HashPair hp) {
    self->eraseBlank(iter, hp);
    rethrow_current_exception();
  }

  void adjustSizeAndBeginBeforeErase(ItemIter iter) {
    sizeAndChunkShiftAndPackedBegin_.decrementSize();
    if constexpr (kEnableItemIteration) {
      if (iter.pack() == sizeAndChunkShiftAndPackedBegin_.packedBegin()) {
        if (size() == 0) {
          iter = ItemIter{};
        } else {
          iter.precheckedAdvance();
        }
        sizeAndChunkShiftAndPackedBegin_.packedBegin() = iter.pack();
      }
    }
  }

  template <typename... Args>
  void insertAtBlank(ItemIter pos, HashPair hp, Args&&... args) {
    auto dst = pos.itemAddr();
    catch_exception(
        [&] {
          this->constructValueAtItem(*this, dst, std::forward<Args>(args)...);
        },
        &eraseBlankCold,
        this,
        pos,
        hp);
    adjustSizeAndBeginAfterInsert(pos);
  }

  ItemIter allocateTag(uint8_t* fullness, HashPair hp) {
    ChunkPtr chunk;
    std::size_t index = hp.first;
    std::size_t delta = probeDelta(hp);
    uint8_t hostedOp = 0;
    while (true) {
      index = moduloByChunkCount(index);
      chunk = chunkAt(index);
      if (fullness[index] < Chunk::kCapacity) [[likely]] {
        break;
      }
      chunk->incrOutboundOverflowCount();
      hostedOp = Chunk::kIncrHostedOverflowCount;
      index += delta;
    }
    unsigned itemIndex = fullness[index]++;
    FOLLY_SAFE_DCHECK(!chunk->occupied(itemIndex), "");
    chunk->setTag(itemIndex, hp.second);
    chunk->adjustHostedOverflowCount(hostedOp);
    return ItemIter{chunk, itemIndex};
  }

  ChunkPtr lastOccupiedChunk() const {
    FOLLY_SAFE_DCHECK(size() > 0, "");
    if constexpr (kEnableItemIteration) {
      return begin().chunk();
    } else {
      return chunkAt(chunkCount() - 1);
    }
  }

  template <typename T>
  void directBuildFrom(T&& src) {
    FOLLY_SAFE_DCHECK(src.size() > 0 && chunkShift() == src.chunkShift(), "");

    // We use std::forward<T> to allow portions of src to be moved out by
    // either beforeBuild or afterBuild, but we are just relying on good
    // behavior of our Policy superclass to ensure that any particular
    // field of this is a donor at most once.

    auto undoState =
        this->beforeBuild(src.size(), bucket_count(), std::forward<T>(src));
    bool success = false;
    SCOPE_EXIT {
      this->afterBuild(
          undoState, success, src.size(), bucket_count(), std::forward<T>(src));
    };

    // Copy can fail part-way through if a Value copy constructor throws.
    // Failing afterBuild is limited in its cleanup power in this case,
    // because it can't enumerate the items that were actually copied.
    // Fortunately we can divide the situation into cases where all of
    // the state is owned by the table itself (F14Node and F14Value),
    // for which clearImpl() can do partial cleanup, and cases where all
    // of the values are owned by the policy (F14Vector), in which case
    // partial failure should not occur.  Sorry for the subtle invariants
    // in the Policy API.

    if (std::is_trivially_copyable<Item>::value &&
        !this->destroyItemOnClear() && itemCount() == src.itemCount()) {
      FOLLY_SAFE_DCHECK(chunkShift() == src.chunkShift(), "");

      auto scale = Chunk::capacityScale(chunksToAddress());

      // most happy path
      auto n = chunkAllocSize(chunkCount(), scale);
      std::memcpy(chunksToAddress(), src.chunksToAddress(), n);
      sizeAndChunkShiftAndPackedBegin_.setSize(src.size());
      if constexpr (kEnableItemIteration) {
        auto srcBegin = src.begin();
        auto srcChunkIdx =
            Chunk::chunkRawIndex(src.chunksToAddress(), &*srcBegin.chunk());
        sizeAndChunkShiftAndPackedBegin_.packedBegin() =
            ItemIter{chunkAt(srcChunkIdx), srcBegin.index()}.pack();
      }
      if constexpr (kContinuousCapacity) {
        // capacityScale might not match even if itemCount matches
        chunks()->setCapacityScale(scale);
      }
    } else {
      // Happy path, no rehash but pack items toward bottom of chunk
      // and use copy constructor.  Don't try to optimize by using
      // lastOccupiedChunk() because there may be higher unoccupied chunks
      // with the overflow bit set.
      auto* srcChunk =
          Chunk::chunkRawAt(src.chunksToAddress(), chunkCount() - 1);
      Chunk* dstChunk = Chunk::chunkRawAt(chunksToAddress(), chunkCount() - 1);
      do {
        dstChunk->copyOverflowInfoFrom(*srcChunk);

        auto iter = srcChunk->occupiedIter();
        if (prefetchBeforeCopy()) {
          for (auto piter = iter; piter.hasNext();) {
            this->prefetchValue(srcChunk->citem(piter.next()));
          }
        }

        std::size_t dstI = 0;
        for (; iter.hasNext(); ++dstI) {
          auto srcI = iter.next();
          auto&& srcArg =
              std::forward<T>(src).buildArgForItem(srcChunk->item(srcI));
          auto dst = dstChunk->itemAddr(dstI);
          this->constructValueAtItem(
              0, dst, std::forward<decltype(srcArg)>(srcArg));
          dstChunk->setTag(dstI, srcChunk->tag(srcI));
          sizeAndChunkShiftAndPackedBegin_.incrementSize();
        }

        srcChunk = Chunk::prevChunkRaw(srcChunk);
        dstChunk = Chunk::prevChunkRaw(dstChunk);
      } while (size() != src.size());

      // reset doesn't care about packedBegin, so we don't fix it until the end
      if constexpr (kEnableItemIteration) {
        std::size_t maxChunkIndex = Chunk::chunkRawIndex(
            src.chunksToAddress(), &*src.lastOccupiedChunk());
        sizeAndChunkShiftAndPackedBegin_.packedBegin() =
            ItemIter{
                chunkAt(maxChunkIndex),
                chunkAt(maxChunkIndex)->lastOccupied().index()}
                .pack();
      }
    }

    success = true;
  }

  template <typename T>
  void rehashBuildFrom(T&& src) {
    FOLLY_SAFE_DCHECK(src.chunkCount() > chunkCount(), "");

    // 1 byte per chunk means < 1 bit per value temporary overhead
    std::array<uint8_t, 256> stackBuf;
    uint8_t* fullness;
    auto cc = chunkCount();
    if (cc <= stackBuf.size()) {
      fullness = stackBuf.data();
    } else {
      ByteAlloc a{this->alloc()};
      fullness = &*std::allocator_traits<ByteAlloc>::allocate(a, cc);
    }
    SCOPE_EXIT {
      if (cc > stackBuf.size()) {
        ByteAlloc a{this->alloc()};
        std::allocator_traits<ByteAlloc>::deallocate(
            a,
            std::pointer_traits<typename std::allocator_traits<
                ByteAlloc>::pointer>::pointer_to(*fullness),
            cc);
      }
    };
    std::memset(fullness, '\0', cc);

    // We use std::forward<T> to allow portions of src to be moved out by
    // either beforeBuild or afterBuild, but we are just relying on good
    // behavior of our Policy superclass to ensure that any particular
    // field of this is a donor at most once.

    // Exception safety requires beforeBuild to happen after all of the
    // allocate() calls.
    auto undoState =
        this->beforeBuild(src.size(), bucket_count(), std::forward<T>(src));
    bool success = false;
    SCOPE_EXIT {
      this->afterBuild(
          undoState, success, src.size(), bucket_count(), std::forward<T>(src));
    };

    // The current table is at a valid state at all points for policies
    // in which non-trivial values are owned by the main table (F14Node
    // and F14Value), so reset() will clean things up properly if we
    // fail partway through.  For the case that the policy manages value
    // lifecycle (F14Vector) then nothing after beforeBuild can throw and
    // we don't have to worry about partial failure.

    std::size_t srcChunkIndex =
        Chunk::chunkRawIndex(src.chunksToAddress(), &*src.lastOccupiedChunk());
    while (true) {
      auto* srcChunk = Chunk::chunkRawAt(src.chunksToAddress(), srcChunkIndex);
      auto iter = srcChunk->occupiedIter();
      if (prefetchBeforeRehash()) {
        for (auto piter = iter; piter.hasNext();) {
          this->prefetchValue(srcChunk->item(piter.next()));
        }
      }
      if (srcChunk->hostedOverflowCount() == 0) {
        // all items are in their preferred chunk (no probing), so we
        // don't need to compute any hash values
        while (iter.hasNext()) {
          auto i = iter.next();
          auto& srcItem = srcChunk->item(i);
          auto&& srcArg = std::forward<T>(src).buildArgForItem(srcItem);
          HashPair hp{srcChunkIndex, srcChunk->tag(i)};
          insertAtBlank(
              allocateTag(fullness, hp),
              hp,
              std::forward<decltype(srcArg)>(srcArg));
        }
      } else {
        // any chunk's items might be in here
        while (iter.hasNext()) {
          auto i = iter.next();
          auto& srcItem = srcChunk->item(i);
          auto&& srcArg = std::forward<T>(src).buildArgForItem(srcItem);
          auto const& srcKey = src.keyForValue(srcArg);
          auto hp = computeHash(srcKey);
          FOLLY_SAFE_CHECK(hp.second == srcChunk->tag(i), "");
          insertAtBlank(
              allocateTag(fullness, hp),
              hp,
              std::forward<decltype(srcArg)>(srcArg));
        }
      }
      if (srcChunkIndex == 0) {
        break;
      }
      --srcChunkIndex;
    }

    success = true;
  }

  [[FOLLY_ATTR_GNU_COLD]] FOLLY_NOINLINE static void buildFromF14TableCatchCold(
      F14Table* self) {
    self->reset();
    F14LinkCheck<getF14IntrinsicsMode()>::check();
    rethrow_current_exception();
  }

  template <typename T>
  FOLLY_NOINLINE void buildFromF14Table(T&& src) {
    FOLLY_SAFE_DCHECK(bucket_count() == 0, "");
    if (src.size() == 0) {
      return;
    }

    // Use the source's capacity, unless it is oversized.
    auto upperLimit = computeChunkCountAndScale(src.size(), false, false);
    auto ccas = std::make_pair(
        src.chunkCount(), Chunk::capacityScale(src.chunksToAddress()));
    FOLLY_SAFE_DCHECK(
        ccas.first >= upperLimit.first,
        "rounded chunk count can't be bigger than actual");
    if (ccas.first > upperLimit.first || ccas.second > upperLimit.second) {
      ccas = upperLimit;
    }
    rehashImpl(0, 1, 0, ccas.first, ccas.second);

    catch_exception(
        [&] {
          if (chunkShift() == src.chunkShift()) {
            directBuildFrom(std::forward<T>(src));
          } else {
            rehashBuildFrom(std::forward<T>(src));
          }
        },
        &F14Table::buildFromF14TableCatchCold,
        this);
  }

  void maybeRehash(std::size_t desiredCapacity, bool attemptExact) {
    auto origChunkCount = chunkCount();
    auto origCapacityScale = Chunk::capacityScale(chunksToAddress());
    auto origCapacity = computeCapacity(origChunkCount, origCapacityScale);

    std::size_t newChunkCount;
    std::size_t newCapacityScale;
    std::tie(newChunkCount, newCapacityScale) = computeChunkCountAndScale(
        desiredCapacity, attemptExact, kContinuousCapacity && attemptExact);
    auto newCapacity = computeCapacity(newChunkCount, newCapacityScale);

    if (origCapacity != newCapacity) {
      rehashImpl(
          size(),
          origChunkCount,
          origCapacityScale,
          newChunkCount,
          newCapacityScale);
    }
  }

  void reserveImpl(std::size_t requestedCapacity) {
    const size_t targetCapacity =
        std::max<std::size_t>(requestedCapacity, size());
    if (targetCapacity == 0) {
      reset();
      return;
    }

    // Special case reserve(n) for n <= size() (pseudo "shrink_to_fit")
    if (requestedCapacity <= size()) {
      maybeRehash(targetCapacity, /*attemptExact*/ true);
      return;
    }

    auto origCapacity = bucket_count();

    // Never shrink in order to avoid O(n^2) behavior of repeated reserves
    if (targetCapacity <= origCapacity) {
      return;
    }

    // Large increase? Good chance the capacity is exactly right
    bool attemptExact =
        targetCapacity > origCapacity + ((origCapacity + 7) / 8);
    maybeRehash(targetCapacity, attemptExact);
  }

  FOLLY_NOINLINE void reserveForInsertImpl(
      std::size_t capacityMinusOne,
      std::size_t origChunkCount,
      std::size_t origCapacityScale,
      std::size_t origCapacity) {
    FOLLY_SAFE_DCHECK(capacityMinusOne >= size(), "");
    std::size_t capacity = capacityMinusOne + 1;

    // we want to grow by between 2^0.5 and 2^1.5 ending at a "good"
    // size, so we grow by 2^0.5 and then round up

    // 1.01101_2 = 1.40625
    std::size_t minGrowth = origCapacity + (origCapacity >> 2) +
        (origCapacity >> 3) + (origCapacity >> 5);
    capacity = std::max<std::size_t>(capacity, minGrowth);

    std::size_t newChunkCount;
    std::size_t newCapacityScale;
    std::tie(newChunkCount, newCapacityScale) =
        computeChunkCountAndScale(capacity, false, false);

    FOLLY_SAFE_DCHECK(
        computeCapacity(newChunkCount, newCapacityScale) > origCapacity, "");

    rehashImpl(
        size(),
        origChunkCount,
        origCapacityScale,
        newChunkCount,
        newCapacityScale);
  }

  void initialReserve(std::size_t desiredCapacity) {
    FOLLY_SAFE_DCHECK(size() == 0, "");
    FOLLY_SAFE_DCHECK(chunkShift() == 0, "");
    FOLLY_SAFE_DCHECK(!!chunks_);
    FOLLY_SAFE_DCHECK(Chunk::isEmptyInstance(chunksToAddress()), "");
    if (desiredCapacity == 0) {
      return;
    }

    std::size_t newChunkCount;
    std::size_t newCapacityScale;
    std::tie(newChunkCount, newCapacityScale) = computeChunkCountAndScale(
        desiredCapacity,
        /*continuousSingleChunkCapacity=*/true,
        kContinuousCapacity);
    auto newCapacity = computeCapacity(newChunkCount, newCapacityScale);
    auto newAllocSize = chunkAllocSize(newChunkCount, newCapacityScale);

    BytePtr rawAllocation;
    auto undoState =
        this->beforeRehash(0, 0, newCapacity, newAllocSize, rawAllocation);

    chunks_ = initializeChunks(rawAllocation, newChunkCount, newCapacityScale);

    sizeAndChunkShiftAndPackedBegin_.setChunkCount(newChunkCount);

    this->afterRehash(
        std::move(undoState), true, 0, 0, newCapacity, nullptr, 0);
  }

  void rehashImpl(
      std::size_t origSize,
      std::size_t origChunkCount,
      std::size_t origCapacityScale,
      std::size_t newChunkCount,
      std::size_t newCapacityScale) {
    auto origChunks = chunks();
    auto origCapacity = computeCapacity(origChunkCount, origCapacityScale);
    auto origAllocSize = chunkAllocSize(origChunkCount, origCapacityScale);
    auto newCapacity = computeCapacity(newChunkCount, newCapacityScale);
    auto newAllocSize = chunkAllocSize(newChunkCount, newCapacityScale);

    BytePtr rawAllocation;
    auto undoState = this->beforeRehash(
        origSize, origCapacity, newCapacity, newAllocSize, rawAllocation);
    chunks_ = initializeChunks(rawAllocation, newChunkCount, newCapacityScale);

    sizeAndChunkShiftAndPackedBegin_.setChunkCount(newChunkCount);

    bool success = false;
    SCOPE_EXIT {
      // this SCOPE_EXIT reverts chunks_ and chunkShift if necessary
      BytePtr finishedRawAllocation = nullptr;
      std::size_t finishedAllocSize = 0;
      if (success) [[likely]] {
        if (origCapacity > 0) {
          finishedRawAllocation = std::pointer_traits<BytePtr>::pointer_to(
              *static_cast<uint8_t*>(static_cast<void*>(&*origChunks)));
          finishedAllocSize = origAllocSize;
        }
      } else {
        finishedRawAllocation = rawAllocation;
        finishedAllocSize = newAllocSize;
        chunks_ = origChunks;
        sizeAndChunkShiftAndPackedBegin_.setChunkCount(origChunkCount);
        F14LinkCheck<getF14IntrinsicsMode()>::check();
      }

      this->afterRehash(
          std::move(undoState),
          success,
          origSize,
          origCapacity,
          newCapacity,
          finishedRawAllocation,
          finishedAllocSize);
    };

    if (origSize == 0) {
      // nothing to do
    } else if (origChunkCount == 1 && newChunkCount == 1) {
      // no mask, no chunk scan, no hash computation, no probing
      auto srcChunk = origChunks;
      auto dstChunk = chunks();
      std::size_t srcI = 0;
      std::size_t dstI = 0;
      while (dstI < origSize) {
        if (srcChunk->occupied(srcI)) [[likely]] {
          dstChunk->setTag(dstI, srcChunk->tag(srcI));
          this->moveItemDuringRehash(
              dstChunk->itemAddr(dstI), srcChunk->item(srcI));
          ++dstI;
        }
        ++srcI;
      }
      if constexpr (kEnableItemIteration) {
        sizeAndChunkShiftAndPackedBegin_.packedBegin() =
            ItemIter{dstChunk, dstI - 1}.pack();
      }
    } else {
      // 1 byte per chunk means < 1 bit per value temporary overhead
      std::array<uint8_t, 256> stackBuf;
      uint8_t* fullness;
      if (newChunkCount <= stackBuf.size()) {
        fullness = stackBuf.data();
      } else {
        ByteAlloc a{this->alloc()};
        // may throw
        fullness =
            &*std::allocator_traits<ByteAlloc>::allocate(a, newChunkCount);
      }
      std::memset(fullness, '\0', newChunkCount);
      SCOPE_EXIT {
        if (newChunkCount > stackBuf.size()) {
          ByteAlloc a{this->alloc()};
          std::allocator_traits<ByteAlloc>::deallocate(
              a,
              std::pointer_traits<typename std::allocator_traits<
                  ByteAlloc>::pointer>::pointer_to(*fullness),
              newChunkCount);
        }
      };

      auto* srcChunk =
          Chunk::chunkRawAt(std::to_address(origChunks), origChunkCount - 1);
      std::size_t remaining = origSize;
      // Software pipeline: prefetch each item's destination chunk, then write
      // the item deferred from the previous iteration, so the destination's
      // cold miss overlaps the next item's hash computation.
      Item* pendingItem = nullptr;
      HashPair pendingHp{};
      while (remaining > 0) {
        auto iter = srcChunk->occupiedIter();
        if (prefetchBeforeRehash()) {
          for (auto piter = iter; piter.hasNext();) {
            this->prefetchValue(srcChunk->item(piter.next()));
          }
        }
        while (iter.hasNext()) {
          --remaining;
          auto srcI = iter.next();
          Item& srcItem = srcChunk->item(srcI);
          auto hp = splitHash(
              this->computeItemHash(const_cast<Item const&>(srcItem)));
          FOLLY_SAFE_CHECK(hp.second == srcChunk->tag(srcI), "");
          prefetchAddr(
              Chunk::chunkRawAt(
                  chunksToAddress(), moduloByChunkCount(hp.first)));
          if (pendingItem != nullptr) {
            auto dstIter = allocateTag(fullness, pendingHp);
            this->moveItemDuringRehash(dstIter.itemAddr(), *pendingItem);
          }
          pendingItem = &srcItem;
          pendingHp = hp;
        }
        srcChunk = Chunk::prevChunkRaw(srcChunk);
      }
      if (pendingItem != nullptr) {
        auto dstIter = allocateTag(fullness, pendingHp);
        this->moveItemDuringRehash(dstIter.itemAddr(), *pendingItem);
      }

      if constexpr (kEnableItemIteration) {
        // this code replaces size invocations of adjustSizeAndBeginAfterInsert
        std::size_t i = chunkCount() - 1;
        while (fullness[i] == 0) {
          --i;
        }
        sizeAndChunkShiftAndPackedBegin_.packedBegin() =
            ItemIter{chunkAt(i), std::size_t{fullness[i]} - 1}.pack();
      }
    }

    success = true;
  }

  // Randomization to help expose bugs when running tests in debug or
  // sanitizer builds

  FOLLY_ALWAYS_INLINE void debugModeOnReserve(std::size_t capacity) {
    if constexpr (kIsLibrarySanitizeAddress || kIsDebug) {
      if (capacity > size()) {
        tlsPendingSafeInserts(static_cast<std::ptrdiff_t>(capacity - size()));
      }
    }
  }

  void debugModeSpuriousRehash() {
    auto cc = chunkCount();
    auto ss = Chunk::capacityScale(chunksToAddress());
    rehashImpl(size(), cc, ss, cc, ss);
  }

  // Returns whether it rehashed.
  FOLLY_ALWAYS_INLINE bool debugModeBeforeInsert() {
    // When running under ASAN, we add a spurious rehash with 1/size()
    // probability before every insert.  This means that finding reference
    // stability problems for F14Value and F14Vector is much more likely.
    // The most common pattern that causes this is
    //
    //   auto& ref = map[k1]; map[k2] = foo(ref);
    //
    // One way to fix this is to call map.reserve(N) before such a
    // sequence, where N is the number of keys that might be inserted
    // within the section that retains references plus the existing size.
    if constexpr (kIsLibrarySanitizeAddress) {
      if (!tlsPendingSafeInserts() && size() > 0 &&
          tlsMinstdRand(size()) == 0) {
        debugModeSpuriousRehash();
        return true;
      }
    }
    return false;
  }

  FOLLY_ALWAYS_INLINE void debugModeAfterInsert() {
    if constexpr (kIsLibrarySanitizeAddress || kIsDebug) {
      tlsPendingSafeInserts(-1);
    }
  }

  FOLLY_ALWAYS_INLINE void debugModePerturbSlotInsertOrder(
      ChunkPtr chunk, std::size_t& itemIndex) {
    FOLLY_SAFE_DCHECK(!chunk->occupied(itemIndex), "");
    constexpr bool perturbSlot = FOLLY_F14_PERTURB_INSERTION_ORDER;
    if (perturbSlot && !tlsPendingSafeInserts()) {
      std::size_t e = chunkShift() == 0 ? bucket_count() : Chunk::kCapacity;
      std::size_t i = itemIndex + tlsMinstdRand(e - itemIndex);
      if (!chunk->occupied(i)) {
        itemIndex = i;
      }
    }
  }

 public:
  // user has no control over max_load_factor

  void rehash(std::size_t capacity) { reserve(capacity); }

  void reserve(std::size_t capacity) {
    // We want to support the pattern
    //   map.reserve(map.size() + 2); auto& r1 = map[k1]; auto& r2 = map[k2];
    debugModeOnReserve(capacity);
    FOLLY_SAFE_DCHECK(!!chunks_);
    if (Chunk::isEmptyInstance(chunksToAddress())) {
      initialReserve(capacity);
    } else {
      reserveImpl(capacity);
    }
  }

  // Returns whether it rehashed.  If it returns false, the table was not
  // modified.
  FOLLY_ALWAYS_INLINE bool reserveForInsert(size_t incoming = 1) {
    FOLLY_SAFE_DCHECK(incoming > 0, "");

    auto needed = size() + incoming;
    auto chunkCount_ = chunkCount();
    auto scale = Chunk::capacityScale(chunksToAddress());
    auto existing = computeCapacity(chunkCount_, scale);
    if (needed - 1 >= existing) {
      reserveForInsertImpl(needed - 1, chunkCount_, scale, existing);
      return true;
    }
    return false;
  }

  // Returns pos,true if construct, pos,false if found.  key is only used
  // during the search; all constructor args for an inserted value come
  // from args...  key won't be accessed after args are touched.
  template <typename K, typename... Args>
  std::pair<ItemIter, bool> tryEmplaceValue(K const& key, Args&&... args) {
    const auto hp = computeHash(key);
    return tryEmplaceValueImpl(hp, key, std::forward<Args>(args)...);
  }

  template <typename K, typename... Args>
  std::pair<ItemIter, bool> tryEmplaceValueWithToken(
      F14HashToken const& token, K const& key, Args&&... args) {
    FOLLY_SAFE_DCHECK(computeHash(key) == static_cast<HashPair>(token), "");
    return tryEmplaceValueImpl(
        static_cast<HashPair>(token), key, std::forward<Args>(args)...);
  }

  template <typename K, typename... Args>
  std::pair<ItemIter, bool> tryEmplaceValueImpl(
      HashPair hp, K const& key, Args&&... args) {
    // In the common case the key lands in its home chunk and this is the only
    // load of a chunk header: it serves the duplicate check below, the
    // first-empty scan, and the tag store.
    std::size_t index = hp.first;
    ChunkPtr chunk = chunkAt(moduloByChunkCount(index));
    auto word = chunk->loadWord();

    if (size() > 0) {
      // Fast path for an absent key, the common case: if the home chunk holds
      // no matching tag and never turned anyone away, findImpl() would stop
      // after its first iteration, so skip it.
      if (chunk->tagMatchIter(word, loadNeedleV(hp.second)).second ||
          chunk->outboundOverflowCount(word) != 0) [[unlikely]] {
        auto existing = findImpl(hp, key, Prefetch::ENABLED);
        if (!existing.atEnd()) {
          return std::make_pair(existing, false);
        }
      }
    }

    // A rehash moves the chunks, so anything loaded above is stale.
    const bool debugRehashed = debugModeBeforeInsert();
    const bool grew = reserveForInsert();
    if (debugRehashed || grew) [[unlikely]] {
      chunk = chunkAt(moduloByChunkCount(index));
      word = chunk->loadWord();
    }

    auto firstEmpty = chunk->firstEmpty(word);

    if (!firstEmpty.hasIndex()) {
      std::size_t delta = probeDelta(hp);
      do {
        chunk->incrOutboundOverflowCount();
        index += delta;
        chunk = chunkAt(moduloByChunkCount(index));
        firstEmpty = chunk->firstEmpty();
      } while (!firstEmpty.hasIndex());
      chunk->adjustHostedOverflowCount(Chunk::kIncrHostedOverflowCount);
      word = chunk->loadWord(); // reload: the line above wrote the header
    }
    std::size_t itemIndex = firstEmpty.index();

    debugModePerturbSlotInsertOrder(chunk, itemIndex);

    chunk->setTag(word, itemIndex, hp.second);
    ItemIter iter{chunk, itemIndex};

    // insertAtBlank will clear the tag if the constructor throws
    insertAtBlank(iter, hp, std::forward<Args>(args)...);

    debugModeAfterInsert();

    return std::make_pair(iter, true);
  }

 private:
  template <bool Reset>
  void clearImpl() noexcept {
    FOLLY_SAFE_DCHECK(!!chunks_);
    if (Chunk::isEmptyInstance(chunksToAddress())) {
      FOLLY_SAFE_DCHECK(empty() && bucket_count() == 0, "");
      return;
    }

    // turn clear into reset if the table is >= 16 chunks so that
    // we don't get too low a load factor
    bool willReset = Reset || chunkCount() >= 16;

    auto origSize = size();
    auto origCapacity = bucket_count();
    if (willReset) {
      this->beforeReset(origSize, origCapacity);
    } else {
      this->beforeClear(origSize, origCapacity);
    }

    if (!empty()) {
      auto* const chunks = chunksToAddress(); // hoisted loop-invariant
      auto const cc = chunkCount(); // hoisted loop-invariant
      if (destroyItemOnClear()) {
        for (std::size_t ci = 0; ci < cc; ++ci) {
          ChunkPtr chunk = std::pointer_traits<ChunkPtr>::pointer_to(
              *Chunk::chunkRawAt(chunks, ci));
          auto iter = chunk->occupiedIter();
          if (prefetchBeforeDestroy()) {
            for (auto piter = iter; piter.hasNext();) {
              this->prefetchValue(chunk->item(piter.next()));
            }
          }
          while (iter.hasNext()) {
            this->destroyItem(chunk->item(iter.next()));
          }
        }
      }
      if (!willReset) {
        // It's okay to do this in a separate loop because we only do it
        // when the chunk count is small.  That avoids a branch when we
        // are promoting a clear to a reset for a large table.
        auto scale = Chunk::capacityScale(chunks);
        for (std::size_t ci = 0; ci < cc; ++ci) {
          Chunk::chunkRawAt(chunks, ci)->clear();
        }
        chunkAt(0)->markEof(scale);
      }
      if constexpr (kEnableItemIteration) {
        sizeAndChunkShiftAndPackedBegin_.packedBegin() = ItemIter{}.pack();
      }
      sizeAndChunkShiftAndPackedBegin_.setSize(0);
    }

    if (willReset) {
      BytePtr rawAllocation = std::pointer_traits<BytePtr>::pointer_to(
          *static_cast<uint8_t*>(static_cast<void*>(&*chunks_)));
      std::size_t rawSize =
          chunkAllocSize(chunkCount(), Chunk::capacityScale(chunksToAddress()));

      chunks_ = Chunk::getSomeEmptyInstance();
      sizeAndChunkShiftAndPackedBegin_.setChunkCount(1);

      this->afterReset(origSize, origCapacity, rawAllocation, rawSize);
    } else {
      this->afterClear(origSize, origCapacity);
    }
  }

  void eraseImpl(ItemIter pos, HashPair hp) {
    this->destroyItem(pos.item());
    adjustSizeAndBeginBeforeErase(pos);
    eraseBlank(pos, hp);
  }

 public:
  // The item needs to still be hashable during this call.  If you want
  // to intercept the value before it is destroyed (to extract it, for
  // example), do so in the beforeDestroy callback.
  template <typename BeforeDestroy>
  void eraseIterInto(ItemIter pos, BeforeDestroy&& beforeDestroy) {
    HashPair hp{};
    if (pos.chunk()->hostedOverflowCount() != 0) {
      hp = splitHash(this->computeItemHash(pos.citem()));
    }
    beforeDestroy(this->valueAtItemForExtract(pos.item()));
    eraseImpl(pos, hp);
  }

  template <typename K, typename BeforeDestroy>
  std::size_t eraseKeyInto(K const& key, BeforeDestroy&& beforeDestroy) {
    if (size() == 0) [[unlikely]] {
      return 0;
    }
    auto hp = computeHash(key);
    auto iter = findImpl(hp, key, Prefetch::ENABLED);
    if (!iter.atEnd()) {
      beforeDestroy(this->valueAtItemForExtract(iter.item()));
      eraseImpl(iter, hp);
      return 1;
    } else {
      return 0;
    }
  }

  void clear() noexcept {
    if constexpr (kIsLibrarySanitizeAddress) {
      // force recycling of heap memory
      auto bc = bucket_count();
      reset();
      catch_exception<std::bad_alloc const&>(
          [this, bc]() { reserveImpl(bc); },
          &folly::detail::thunk::noop<std::bad_alloc const&>);
    } else {
      clearImpl<false>();
    }
  }

  // Like clear(), but always frees all dynamic storage allocated
  // by the table.
  void reset() noexcept { clearImpl<true>(); }

  // Get memory footprint, not including sizeof(*this).
  std::size_t getAllocatedMemorySize() const {
    std::size_t sum = 0;
    visitAllocationClasses([&sum](std::size_t bytes, std::size_t n) {
      sum += bytes * n;
    });
    return sum;
  }

  // Enumerates classes of allocated memory blocks currently owned
  // by this table, calling visitor(allocationSize, allocationCount).
  // This can be used to get a more accurate indication of memory footprint
  // than getAllocatedMemorySize() if you have some way of computing the
  // internal fragmentation of the allocator, such as JEMalloc's nallocx.
  // The visitor might be called twice with the same allocationSize. The
  // visitor's computation should produce the same result for visitor(8,
  // 2) as for two calls to visitor(8, 1), for example.  The visitor may
  // be called with a zero allocationCount.
  template <typename V>
  void visitAllocationClasses(V&& visitor) const {
    auto scale = Chunk::capacityScale(chunksToAddress());
    this->visitPolicyAllocationClasses(
        scale == 0 ? 0 : chunkAllocSize(chunkCount(), scale),
        size(),
        bucket_count(),
        visitor);
  }

  // visitor should take an Item const&
  template <typename V>
  void visitItems(V&& visitor) const {
    if (empty()) {
      return;
    }
    std::size_t maxChunkIndex = chunkIndex(&*lastOccupiedChunk());
    auto* chunk = chunksToAddress();
    for (std::size_t i = 0; i <= maxChunkIndex;
         ++i, chunk = Chunk::nextChunkRaw(chunk)) {
      auto iter = chunk->occupiedIter();
      if (prefetchBeforeCopy()) {
        for (auto piter = iter; piter.hasNext();) {
          this->prefetchValue(chunk->citem(piter.next()));
        }
      }
      while (iter.hasNext()) {
        visitor(chunk->citem(iter.next()));
      }
    }
  }

  // visitor should take two Item const*
  template <typename V>
  void visitContiguousItemRanges(V&& visitor) const {
    if (empty()) {
      return;
    }
    std::size_t maxChunkIndex = chunkIndex(&*lastOccupiedChunk());
    auto* chunk = chunksToAddress();
    for (std::size_t i = 0; i <= maxChunkIndex;
         ++i, chunk = Chunk::nextChunkRaw(chunk)) {
      for (auto iter = chunk->occupiedRangeIter(); iter.hasNext();) {
        auto be = iter.next();
        FOLLY_SAFE_DCHECK(
            chunk->occupied(be.first) && chunk->occupied(be.second - 1), "");
        Item const* b = chunk->itemAddr(be.first);
        visitor(b, b + (be.second - be.first));
      }
    }
  }

 private:
  static std::size_t& histoAt(
      std::vector<std::size_t>& histo, std::size_t index) {
    if (histo.size() <= index) {
      histo.resize(index + 1);
    }
    return histo.at(index);
  }

 public:
  // Expensive
  F14TableStats computeStats() const {
    F14TableStats stats;

    if constexpr (kIsDebug && kEnableItemIteration) {
      // validate iteration
      std::size_t n = 0;
      ItemIter prev;
      for (auto iter = begin(); iter != end(); iter.advance()) {
        FOLLY_SAFE_DCHECK(n == 0 || iter.pack() < prev.pack(), "");
        ++n;
        prev = iter;
      }
      FOLLY_SAFE_DCHECK(n == size(), "");
    }

    FOLLY_SAFE_DCHECK(
        (Chunk::isEmptyInstance(chunksToAddress())) == (bucket_count() == 0),
        "");

    std::size_t n1 = 0;
    std::size_t n2 = 0;
    auto cc = bucket_count() == 0 ? 0 : chunkCount();
    for (std::size_t ci = 0; ci < cc; ++ci) {
      ChunkPtr chunk = chunkAt(ci);
      FOLLY_SAFE_DCHECK(chunk->eof() == (ci == 0), "");

      auto iter = chunk->occupiedIter();

      std::size_t chunkOccupied = 0;
      for (auto piter = iter; piter.hasNext(); piter.next()) {
        ++chunkOccupied;
      }
      n1 += chunkOccupied;

      histoAt(stats.chunkOccupancyHisto, chunkOccupied)++;
      histoAt(
          stats.chunkOutboundOverflowHisto, chunk->outboundOverflowCount())++;
      histoAt(stats.chunkHostedOverflowHisto, chunk->hostedOverflowCount())++;

      while (iter.hasNext()) {
        auto ii = iter.next();
        ++n2;

        {
          auto& item = chunk->citem(ii);
          auto hp = splitHash(this->computeItemHash(item));
          FOLLY_SAFE_DCHECK(chunk->tag(ii) == hp.second, "");

          std::size_t dist = 1;
          std::size_t index = hp.first;
          std::size_t delta = probeDelta(hp);
          while (moduloByChunkCount(index) != ci) {
            index += delta;
            ++dist;
          }

          histoAt(stats.keyProbeLengthHisto, dist)++;
        }

        // misses could have any tag, so we do the dumb but accurate
        // thing and just try them all
        for (std::size_t ti = 1; ti < 256; ++ti) {
          uint8_t tag = static_cast<uint8_t>(ti);
          HashPair hp{ci, tag};

          std::size_t dist = 1;
          std::size_t index = hp.first;
          std::size_t delta = probeDelta(hp);
          for (std::size_t tries = 0; tries >> chunkShift() == 0 &&
               chunkAt(moduloByChunkCount(index))->outboundOverflowCount() != 0;
               ++tries) {
            index += delta;
            ++dist;
          }

          histoAt(stats.missProbeLengthHisto, dist)++;
        }
      }
    }

    FOLLY_SAFE_DCHECK(n1 == size(), "");
    FOLLY_SAFE_DCHECK(n2 == size(), "");

    stats.policy = pretty_name<Policy>();
    stats.size = size();
    stats.valueSize = sizeof(value_type);
    stats.bucketCount = bucket_count();
    stats.chunkCount = cc;

    stats.totalBytes = sizeof(*this) + getAllocatedMemorySize();
    stats.overheadBytes = stats.totalBytes - size() * sizeof(value_type);

    return stats;
  }
};
} // namespace detail
} // namespace f14

#endif // FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

namespace f14 {
namespace test {
inline void disableInsertOrderRandomization() {
  if constexpr (kIsLibrarySanitizeAddress || kIsDebug) {
    detail::tlsPendingSafeInserts(
        static_cast<std::ptrdiff_t>(
            (std::numeric_limits<std::size_t>::max)() / 2));
  }
}
} // namespace test
} // namespace f14
} // namespace folly
