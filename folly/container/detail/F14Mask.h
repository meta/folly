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

#include <algorithm>
#include <cstdint>

#include <folly/Bits.h>
#include <folly/ConstexprMath.h>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/container/detail/F14IntrinsicsAvailability.h>
#include <folly/lang/Assume.h>
#include <folly/lang/SafeAssert.h>

// This ISA choice may depend only on the target ISA, which must be the same in
// every file of a program (as must any -D override of this macro). It must
// never depend on macros that can differ between files of one binary, such as
// __OPTIMIZE__. The linker usually keeps one out-of-line copy of each inline
// F14 function for the whole program, so every copy must be interchangeable:
// same types, signatures and return layout, and the same meaning for the table
// data. An -O0 library that took the NEON path inside an optimized SVE2 binary
// once aborted every process at startup. Copies may differ only in ways every
// copy agrees on: inlining (FOLLY_F14_SVE_INLINE), or debug-only checks and
// insertion-slot perturbation under kIsDebug.
#ifndef FOLLY_F14_DETAIL_ISA_SVE2_WITH_BRIDGE
#if FOLLY_ARM_FEATURE_NEON_SVE_BRIDGE && FOLLY_ARM_FEATURE_SVE2
#define FOLLY_F14_DETAIL_ISA_SVE2_WITH_BRIDGE 1
#else
#define FOLLY_F14_DETAIL_ISA_SVE2_WITH_BRIDGE 0
#endif
#endif

#if FOLLY_F14_VECTOR_INTRINSICS_AVAILABLE

namespace folly {
namespace f14 {
namespace detail {

template <typename T>
FOLLY_ALWAYS_INLINE static unsigned findFirstSetNonZero(T mask) {
  assume(mask != 0);
  if (sizeof(mask) == sizeof(unsigned)) {
    return __builtin_ctz(static_cast<unsigned>(mask));
  } else {
    return __builtin_ctzll(mask);
  }
}

template <typename T>
FOLLY_ALWAYS_INLINE static unsigned findLastSetNonZero(T mask) {
  assume(mask != 0);
  if (sizeof(mask) == sizeof(unsigned)) {
    return __builtin_clz(static_cast<unsigned>(mask));
  } else {
    return __builtin_clzll(mask);
  }
}

#if FOLLY_NEON
using MaskType = uint64_t;

constexpr unsigned kMaskSpacing = 4;
#else // FOLLY_SSE >= 2 || FOLLY_RISCV64
using MaskType = uint32_t;

constexpr unsigned kMaskSpacing = 1;
#endif

template <unsigned BitCount>
struct FullMask {
  static constexpr MaskType value =
      (FullMask<BitCount - 1>::value << kMaskSpacing) + 1;
};

template <>
struct FullMask<1> : std::integral_constant<MaskType, 1> {};

#if FOLLY_ARM
// Mask iteration is different for ARM because that is the only platform
// for which the mask is bigger than a register.

// Iterates a mask, optimized for the case that only a few bits are set
class SparseMaskIter {
  static_assert(kMaskSpacing == 4);

  uint32_t interleavedMask_;

 public:
  explicit SparseMaskIter(MaskType mask)
      : interleavedMask_{static_cast<uint32_t>(((mask >> 32) << 2) | mask)} {}

  bool hasNext() { return interleavedMask_ != 0; }

  unsigned next() {
    FOLLY_SAFE_DCHECK(hasNext(), "");
    unsigned i = findFirstSetNonZero(interleavedMask_);
    interleavedMask_ &= (interleavedMask_ - 1);
    return ((i >> 2) | (i << 2)) & 0xf;
  }
};

// Iterates a mask, optimized for the case that most bits are set
class DenseMaskIter {
  static_assert(kMaskSpacing == 4);

  std::size_t count_;
  unsigned index_;
  uint8_t const* tags_;

 public:
  explicit DenseMaskIter(uint8_t const* tags, MaskType mask) {
    if (mask == 0) {
      count_ = 0;
    } else {
      count_ = popcount(static_cast<uint32_t>(((mask >> 32) << 2) | mask));
      if ((mask & 1) != 0) [[likely]] {
        index_ = 0;
      } else {
        index_ = findFirstSetNonZero(mask) / kMaskSpacing;
      }
      tags_ = tags;
    }
  }

  bool hasNext() { return count_ > 0; }

  unsigned next() {
    auto rv = index_;
    --count_;
    if (count_ > 0) {
      do {
        ++index_;
      } while (tags_[index_] == 0);
    }
    FOLLY_SAFE_DCHECK(index_ < 16, "");
    return rv;
  }
};

#else
// Iterates a mask, optimized for the case that only a few bits are set
class SparseMaskIter {
  MaskType mask_;

 public:

#if FOLLY_AARCH64
  explicit SparseMaskIter(MaskType mask) : mask_{bitReverse(mask)} {}
#else
  explicit SparseMaskIter(MaskType mask) : mask_{mask} {}
#endif

  bool hasNext() { return mask_ != 0; }

  unsigned next() {
    FOLLY_SAFE_DCHECK(hasNext(), "");
    constexpr uint64_t lo63 = 0x7FFFFFFFFFFFFFFFull;
    static_assert(lo63 == (~0ull >> 1));
    unsigned i =
        kIsArchAArch64 ? findLastSetNonZero(mask_) : findFirstSetNonZero(mask_);
    mask_ &= kIsArchAArch64 ? (lo63 >> i) : (mask_ - 1);
    if constexpr (kIsArchAArch64 && (kMaskSpacing == 4)) {
      // The result of this function is often used as an index on an 8-byte
      // element array. In this case, the index needs to be shifted left by 3 to
      // access the desired memory position. The return statement of this
      // function contains i >> 2. The compiler is simplifying the shifts by
      // only issuing a lsl 1 while ommitting the lsr 2. However, it then ANDs
      // the shifted value by 0xf8, to ensure correctness when i is not a
      // multiple of 4. We do know that i will always be a multiple of 4. We add
      // the assume clause so the compiler avoids emitting the &0xf8
      auto loadIndex = i << 1;
      assume(loadIndex == (loadIndex & 0xf8));
    }
    return i / kMaskSpacing;
  }
};

// A variant of SparseMaskIter which tests the index of the next set bit against
// a bound instead of the bitvector itself.
template <unsigned BitCount>
class BoundedMaskIter {
  static_assert(BitCount < sizeof(MaskType) * 8);

  MaskType mask_;

 public:
  explicit BoundedMaskIter(MaskType mask) : mask_{mask} {}

  bool hasNext() {
    // true iff mask_ has a set bit below BitCount, so next() will return
    // an in-bounds index
    return (mask_ & FullMask<BitCount>::value) != 0;
  }

  unsigned next() {
    FOLLY_SAFE_DCHECK(hasNext());
    unsigned i = findFirstSetNonZero(mask_);
    mask_ &= mask_ - 1;
    return i;
  }
};

// Iterates a mask, optimized for the case that most bits are set
class DenseMaskIter {
  MaskType mask_;
  unsigned index_{0};

 public:
  explicit DenseMaskIter(uint8_t const*, MaskType mask) : mask_{mask} {}

  bool hasNext() { return mask_ != 0; }

  unsigned next() {
    FOLLY_SAFE_DCHECK(hasNext(), "");
    if ((mask_ & 1) != 0) [[likely]] {
      mask_ >>= kMaskSpacing;
      return index_++;
    } else {
      unsigned s = findFirstSetNonZero(mask_);
      unsigned rv = index_ + (s / kMaskSpacing);
      mask_ >>= (s + kMaskSpacing);
      index_ = rv + 1;
      return rv;
    }
  }
};
#endif

// Iterates a mask, returning pairs of [begin,end) index covering blocks
// of set bits
class MaskRangeIter {
  MaskType mask_;
  unsigned shift_{0};

 public:
  explicit MaskRangeIter(MaskType mask) {
    // If kMaskSpacing is > 1 then there will be empty bits even for
    // contiguous ranges.  Fill them in.
    mask_ = mask * ((1 << kMaskSpacing) - 1);
  }

  bool hasNext() { return mask_ != 0; }

  std::pair<unsigned, unsigned> next() {
    FOLLY_SAFE_DCHECK(hasNext(), "");
    auto s = shift_;
    unsigned b = findFirstSetNonZero(mask_);
    unsigned e = findFirstSetNonZero(~(mask_ | (mask_ - 1)));
    mask_ >>= e;
    shift_ = s + e;
    return std::make_pair((s + b) / kMaskSpacing, (s + e) / kMaskSpacing);
  }
};

// Holds the result of an index query that has an optional result,
// interpreting a mask of 0 to be the empty answer and the index of the
// last set bit to be the non-empty answer
class LastOccupiedInMask {
  MaskType mask_;

 public:
  explicit LastOccupiedInMask(MaskType mask) : mask_{mask} {}

  bool hasIndex() const { return mask_ != 0; }

  unsigned index() const {
    assume(mask_ != 0);
    return (findLastSet(mask_) - 1) / kMaskSpacing;
  }
};

// Holds the result of an index query that has an optional result,
// interpreting a mask of 0 to be the empty answer and the index of the
// first set bit to be the non-empty answer
class FirstEmptyInMask {
  MaskType mask_;

 public:
  explicit FirstEmptyInMask(MaskType mask) : mask_{mask} {}

  bool hasIndex() const { return mask_ != 0; }

  unsigned index() const {
    FOLLY_SAFE_DCHECK(mask_ != 0, "");
    return findFirstSetNonZero(mask_) / kMaskSpacing;
  }
};

// Same interface as FirstEmptyInMask, but already resolved rather than
// holding a mask to search. For callers that compute the index directly
// (e.g. via an SVE predicate, which cannot be stored in a mask the way
// FirstEmptyInMask requires) instead of via a packed MaskType.
class ResolvedFirstEmpty {
  bool hasIndex_;
  // Matches svcntp_b8's uint64_t return type, avoiding a truncate/repack.
  std::size_t index_;

 public:
  ResolvedFirstEmpty(bool hasIndex, std::size_t index)
      : hasIndex_{hasIndex}, index_{index} {}

  bool hasIndex() const { return hasIndex_; }

  unsigned index() const {
    FOLLY_SAFE_DCHECK(hasIndex_, "");
    return static_cast<unsigned>(index_);
  }
};

} // namespace detail
} // namespace f14
} // namespace folly

#endif
