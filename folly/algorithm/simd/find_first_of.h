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
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include <folly/Portability.h>
#include <folly/Utility.h>
#include <folly/algorithm/simd/Movemask.h>
#include <folly/container/SparseByteSet.h>
#include <folly/lang/Align.h>
#include <folly/lang/Bits.h>
#include <folly/lang/Hint.h>

#if FOLLY_SSE
#include <immintrin.h>
#endif

#if FOLLY_NEON
#include <arm_neon.h>
#endif

#if FOLLY_ARM_FEATURE_SVE
#include <arm_sve.h>
#if __has_include(<arm_neon_sve_bridge.h>)
#include <arm_neon_sve_bridge.h> // @manual
#endif
#endif

namespace folly::simd {

namespace detail {

/// stdfind_scalar_finder_first_of
///
/// A find-first-of finder which simply wraps std::find.
template <typename CharT>
class stdfind_scalar_finder_first_of {
 private:
  using value_type = CharT;
  using view = std::span<CharT const>;

  alignas(sizeof(view)) view const alphabet_;

 public:
  constexpr explicit stdfind_scalar_finder_first_of(
      view const alphabet) noexcept
      : alphabet_{alphabet} {}

  size_t operator()(view const input, size_t const pos = 0) const noexcept {
    auto const r = std::find_first_of(
        input.subspan(pos).begin(),
        input.end(),
        alphabet_.begin(),
        alphabet_.end());
    return r - input.begin();
  }
};

/// default_scalar_finder_first_of
///
/// A find-first-of finder which, for each element of the input, iterates the
/// search alphabet. Has complexity O(MN), with M the length of the alphabet and
/// with N the length of the input.
///
/// Requires no precomputation or storage.
template <typename CharT, bool Eq>
class default_scalar_finder_first_op_of {
 private:
  using value_type = CharT;
  using view = std::span<CharT const>;

  alignas(sizeof(view)) view alphabet_;

  bool match(value_type const c) const noexcept {
    bool ret = !Eq;
    for (auto const a : alphabet_) {
      auto const v = a == c;
      ret = Eq ? ret || v : ret && !v;
    }
    return ret;
  }

 public:
  constexpr explicit default_scalar_finder_first_op_of(
      view const alphabet) noexcept
      : alphabet_{alphabet} {}

  size_t operator()(view const input, size_t const pos = 0) const noexcept {
    for (size_t i = pos; i < input.size(); ++i) {
      if (match(input[i])) {
        return i;
      }
    }
    return input.size();
  }
};

/// ltindex_scalar_finder_first_of
///
/// A find-first-of finder which, for each element of the input, looks up that
/// element in a lookup table. Has complexity O(N), with N the length of the
/// input.
///
/// Precomputes and stores a 256-byte lookup table. Precomputation has
/// complexity O(M), with M the length of the alphabet.
///
/// Restricted to elements which are 1 byte wide.
template <typename CharT, bool Eq>
class ltindex_scalar_finder_first_op_of {
 private:
  using value_type = CharT;
  using view = std::span<CharT const>;
  using index = std::array<bool, 256>;

  static_assert(sizeof(value_type) == 1);

  alignas(hardware_destructive_interference_size) index const ltindex_;

  static constexpr index make_index(view const alphabet) noexcept {
    index ltindex{};
    for (auto const a : alphabet) {
      ltindex[static_cast<uint8_t>(a)] = true;
    }
    return ltindex;
  }

  bool match(value_type const c) const noexcept {
    return Eq == ltindex_[static_cast<uint8_t>(c)];
  }

 public:
  constexpr explicit ltindex_scalar_finder_first_op_of(
      view const alphabet) noexcept
      : ltindex_{make_index(alphabet)} {}

  size_t operator()(view const input, size_t const pos = 0) const noexcept {
    for (size_t i = pos; i < input.size(); ++i) {
      if (match(input[i])) {
        return i;
      }
    }
    return input.size();
  }
};

/// ltsparse_scalar_finder_first_of
///
/// A find-first-of finder which, for each element of the input, looks up that
/// element in a lookup table. Has complexity O(M+N), with M the length of the
/// alphabet and with N the length of the input.
///
/// Similar to ltindex_scalar_finder_first_of, but where the precomputation is
/// instead done at the beginning of each search using an alternative set type.
/// This alternative set type has lower setup cost but higher lookup cost as
/// compared with the set type in ltindex_scalar_finder_first_of, making this
/// implementation more suitable for unpredictable alphabets.
///
/// Requires no precomputation or storage.
///
/// Restricted to elements which are 1 byte wide.
template <typename CharT, bool Eq>
class ltsparse_scalar_finder_first_op_of {
 private:
  using value_type = CharT;
  using view = std::span<CharT const>;

  static_assert(sizeof(value_type) == 1);

  alignas(sizeof(view)) view alphabet_;

  void prep(SparseByteSet& set) const noexcept {
    for (auto const a : alphabet_) {
      set.add(static_cast<uint8_t>(a));
    }
  }

  bool match(value_type const c, SparseByteSet const& set) const noexcept {
    return Eq == set.contains(static_cast<uint8_t>(c));
  }

 public:
  constexpr explicit ltsparse_scalar_finder_first_op_of(
      view const alphabet) noexcept
      : alphabet_{alphabet} {}

  size_t operator()(view const input, size_t const pos = 0) const noexcept {
    [[FOLLY_ATTR_CLANG_UNINITIALIZED]] SparseByteSet set;
    prep(set);
    for (size_t i = pos; i < input.size(); ++i) {
      if (match(input[i], set)) {
        return i;
      }
    }
    return input.size();
  }
};

/// default_vector_finder_first_of
///
/// A find-first-of finder which, for each element of the input, iterates the
/// search alphabet. Has complexity O(MN), with M the length of the alphabet and
/// with N the length of the input.
///
/// Like default_scalar_finder_first_of, but accelerated with simd instructions
/// to search up to 16 elements of the input at a time.
///
/// Requires no precomputation or storage.
///
/// Restricted to elements which are 1 byte wide.
///
/// Implemented for x86-64 and aarch64 architectures.
///
/// Requires a fallback scalar finder for not-implemented-architecture and for
/// near-end-of-input.
template <typename CharT, bool Eq>
class default_vector_finder_first_op_of {
 private:
  using value_type = CharT;
  using view = std::span<CharT const>;

  static_assert(sizeof(value_type) == 1);

  alignas(sizeof(view)) view const alphabet_;

 public:
  constexpr explicit default_vector_finder_first_op_of(
      view const alphabet) noexcept
      : alphabet_{alphabet} {}

  template <typename Scalar>
  size_t operator()(
      Scalar const& scalar,
      view const input,
      size_t const pos = 0) const noexcept {
    return operator()(scalar, true, input, pos);
  }

  template <typename Scalar>
  size_t operator()(
      Scalar const& scalar,
      bool const vector,
      view const input,
      size_t const pos = 0) const noexcept {
    size_t size = pos;
    if (vector) {
#if (FOLLY_SSE >= 2 || (FOLLY_NEON && FOLLY_AARCH64))
      while (input.size() >= size + 16) {
#if FOLLY_SSE
        auto const vhaystack = _mm_loadu_si128(
            reinterpret_cast<__m128i const*>(input.data() + size));
        auto vmask = _mm_set1_epi8(Eq ? 0 : -1);
        for (auto const a : alphabet_) {
          auto const veq = _mm_cmpeq_epi8(vhaystack, _mm_set1_epi8(a));
          vmask = Eq ? _mm_or_si128(veq, vmask) : _mm_andnot_si128(veq, vmask);
        }
#elif FOLLY_NEON
        auto const vhaystack =
            vld1q_u8(reinterpret_cast<uint8_t const*>(input.data() + size));
        auto vmask = vdupq_n_u8(Eq ? 0 : -1);
        for (auto const a : alphabet_) {
          auto const veq = vhaystack == vdupq_n_u8(a);
          vmask = Eq ? veq | vmask : ~veq & vmask;
        }
#endif
        if (auto const [word, bits] = movemask<CharT>(vmask); word) {
          return size + to_signed((findFirstSet(word) - 1) / bits);
        }
        size += 16;
      }
      if (input.size() < size) {
        compiler_may_unsafely_assume_unreachable();
      }
#endif
    }
    return scalar(input, size);
  }
};

/// shuffle_vector_finder_first_of
///
/// A find-first-of finder which, for each element of the input, looks up that
/// element in a lookup table. Has complexity O(MN), with M the length of the
/// alphabet after deduplication and with N the length of the input.
///
/// Precomputes and stores a 256-byte lookup table. Precomputation has
/// complexity O(M), with M the length of the alphabet.
///
/// Like ltindex_scalar_finder_first_of, but accelerated with simd instructions
/// to search up to 16 elements of the input at a time and decelerated by
/// splitting the lookup table into a sequence of lookup tables of length O(M).
///
/// Restricted to elements which are 1 byte wide.
///
/// Implemented for x86-64 and aarch64 architectures.
///
/// Requires a fallback scalar finder for not-implemented-architecture and for
/// near-end-of-input.
template <typename CharT, bool Eq>
class shuffle_vector_finder_first_op_of {
 private:
  using value_type = CharT;
  using view = std::span<CharT const>;
  using shufvec = std::array<value_type, 256>;

  struct shuffle {
    shufvec table;
    size_t rounds;
  };

  static_assert(sizeof(value_type) == 1);

  //  invariant: (a in alphabet) <=> (exists k : shufvec[k * 16 + a % 16] = a)
  alignas(hardware_destructive_interference_size) shuffle const shuffle_;

  static constexpr shuffle make_shuffle(view const alphabet) noexcept {
    //  init requires: forall k, a : result[k * 16 + a % 16] != a
    shufvec table{1}; // 1, 0, 0, ...
    size_t maxk{};

    std::array<bool, 256> seen{};
    std::array<size_t, 16> lo_seen{};

    for (auto const a : alphabet) {
      auto const v = static_cast<uint8_t>(a);
      if (!std::exchange(seen[v], true)) {
        auto const k = lo_seen[v % 16]++;
        maxk = maxk < k ? k : maxk;
        table[k * 16 + v % 16] = v;
      }
    }

    return {table, maxk + 1};
  }

 public:
  constexpr explicit shuffle_vector_finder_first_op_of(
      view const alphabet) noexcept
      : shuffle_{make_shuffle(alphabet)} {}

  template <typename Scalar>
  size_t operator()(
      Scalar const& scalar,
      view const input,
      size_t const pos = 0) const noexcept {
    return operator()(scalar, true, input, pos);
  }

  template <typename Scalar>
  size_t operator()(
      Scalar const& scalar,
      bool const vector,
      view const input,
      size_t const pos = 0) const noexcept {
    size_t size = pos;
    if (vector) {
#if ((FOLLY_SSE >= 2 && FOLLY_SSSE >= 3) || (FOLLY_NEON && FOLLY_AARCH64))
      auto const table = shuffle_.table.data();
      while (input.size() >= size + 16) {
#if FOLLY_SSE
        auto const vtable = reinterpret_cast<__m128i const*>(table);
        auto const vhaystack = _mm_loadu_si128(
            reinterpret_cast<__m128i const*>(input.data() + size));
        auto const vhaystackm = _mm_and_si128(vhaystack, _mm_set1_epi8(15));
        auto vmask = _mm_set1_epi8(Eq ? 0 : -1);
        for (size_t i = 0; i < shuffle_.rounds; ++i) {
          auto const vshuffle = _mm_shuffle_epi8(vtable[i], vhaystackm);
          auto const veq = _mm_cmpeq_epi8(vshuffle, vhaystack);
          vmask = Eq ? _mm_or_si128(veq, vmask) : _mm_andnot_si128(veq, vmask);
        }
#elif FOLLY_NEON
        auto const vtable = reinterpret_cast<uint8x16_t const*>(table);
        auto const vhaystack =
            vld1q_u8(reinterpret_cast<uint8_t const*>(input.data() + size));
        auto vmask = vdupq_n_u8(Eq ? 0 : -1);
        for (size_t i = 0; i < shuffle_.rounds; ++i) {
          auto const veq = vqtbl1q_u8(vtable[i], vhaystack & 15) == vhaystack;
          vmask = Eq ? veq | vmask : ~veq & vmask;
        }
#endif
        if (auto const [word, bits] = movemask<CharT>(vmask); word) {
          return size + to_signed((findFirstSet(word) - 1) / bits);
        }
        size += 16;
      }
      if (input.size() < size) {
        compiler_may_unsafely_assume_unreachable();
      }
#endif
    }
    return scalar(input, size);
  }
};

/// azmatch_vector_finder_first_of
///
/// A find-first-of finder which, for each element of the input, looks up that
/// element in a lookup table. Has complexity O(MN), with M the length of the
/// alphabet after deduplication and with N the length of the input.
///
/// Precomputes and stores a 256-byte lookup table. Precomputation has
/// complexity O(M), with M the length of the alphabet.
///
/// Like ltindex_scalar_finder_first_of, but accelerated with simd instructions
/// to search up to 16 elements of the input at a time and decelerated by
/// splitting the lookup table into a sequence of lookup tables of length O(M).
///
/// Like ltindex_vector_finder_first_of, but with a different technique.
///
/// Restricted to elements which are 1 byte wide.
///
/// Implemented for aarch64 architectures with sve.
///
/// Requires a fallback scalar finder for not-implemented-architecture and for
/// near-end-of-input.
template <typename CharT, bool Eq>
class azmatch_vector_finder_first_op_of {
 private:
  using value_type = CharT;
  using view = std::span<CharT const>;
  using matchvec = std::array<value_type, 256>;

  struct meta {
    matchvec table;
    size_t rounds;
  };

  static_assert(sizeof(value_type) == 1);

  alignas(hardware_destructive_interference_size) meta const meta_;

  static constexpr size_t next_segment(
      view& alphabet,
      std::span<value_type, 16> out,
      std::span<bool, 256> seen) noexcept {
    if (!alphabet.size()) {
      return 0;
    }
    for (size_t i = 0; i < 16; ++i) {
      out[i] = alphabet[0];
    }
    size_t items = 0;
    while (items < 16 && alphabet.size()) {
      auto const v = static_cast<uint8_t>(alphabet[0]);
      alphabet = alphabet.subspan(1);
      if (!std::exchange(seen[v], true)) {
        out[items++] = v;
      }
    }
    return items;
  }

  static constexpr meta make_meta(view alphabet) noexcept {
    std::array<bool, 256> seen{};
    size_t rounds = 0;
    matchvec vec{};
    while (true) {
      auto segment = std::span<value_type, 16>{vec.data() + 16 * rounds, 16};
      auto segsize = next_segment(alphabet, segment, seen);
      if (!segsize) {
        break;
      }
      ++rounds;
    }
    return meta{vec, rounds};
  }

#if FOLLY_ARM_FEATURE_SVE
  static auto svld1_u8_nopred_16(uint8_t const* p) noexcept {
#if __has_include(<arm_neon_sve_bridge.h>)
    return svset_neonq_u8(svundef_u8(), vld1q_u8(p));
#else
    return svld1_u8(svptrue_pat_b8(SV_VL16), p);
#endif
  }
#endif

 public:
  constexpr explicit azmatch_vector_finder_first_op_of(
      view const alphabet) noexcept
      : meta_{make_meta(alphabet)} {}

  template <typename Scalar>
  size_t operator()(
      Scalar const& scalar,
      view const input,
      size_t const pos = 0) const noexcept {
    return operator()(scalar, true, input, pos);
  }

  template <typename Scalar>
  size_t operator()(
      Scalar const& scalar,
      bool const vector,
      view const input,
      size_t const pos = 0) const noexcept {
    size_t size = pos;
    if (vector) {
#if FOLLY_ARM_FEATURE_SVE
      auto const table = reinterpret_cast<uint8_t const*>(meta_.table.data());
      while (input.size() >= size + 16) {
        auto const pred = svptrue_b8();
        auto const vhaystack = svld1_u8_nopred_16(
            reinterpret_cast<uint8_t const*>(input.data() + size));
        auto vmask = Eq ? svpfalse_b() : pred;
        for (size_t i = 0; i < meta_.rounds; ++i) {
          auto const vsegment = svld1_u8_nopred_16(table + 16 * i);
          vmask = Eq
              ? svorr_b_z(pred, vmask, svmatch_u8(pred, vhaystack, vsegment))
              : svand_b_z(pred, vmask, svnmatch_u8(pred, vhaystack, vsegment));
        }
        // an important optimization that llvm-17 *could*, but doesn't, do for
        // sve
        if (meta_.rounds == 1) {
          auto const vsegment = svld1_u8_nopred_16(table);
          vmask = Eq
              ? svmatch_u8(pred, vhaystack, vsegment)
              : svnmatch_u8(pred, vhaystack, vsegment);
        }
        auto const count = svcntp_b8(pred, svbrkb_b_z(pred, vmask));
        if (count < 16) {
          return size + count;
        }
        size += 16;
      }
      if (input.size() < size) {
        compiler_may_unsafely_assume_unreachable();
      }
#endif
    }
    return scalar(input, size);
  }
};

} // namespace detail

template <typename CharT>
using basic_stdfind_scalar_finder_first_of =
    detail::stdfind_scalar_finder_first_of<CharT>;

template <typename CharT>
using basic_default_scalar_finder_first_of =
    detail::default_scalar_finder_first_op_of<CharT, true>;
template <typename CharT>
using basic_default_scalar_finder_first_not_of =
    detail::default_scalar_finder_first_op_of<CharT, false>;

template <typename CharT>
using basic_ltindex_scalar_finder_first_of =
    detail::ltindex_scalar_finder_first_op_of<CharT, true>;
template <typename CharT>
using basic_ltindex_scalar_finder_first_not_of =
    detail::ltindex_scalar_finder_first_op_of<CharT, false>;

template <typename CharT>
using basic_ltsparse_scalar_finder_first_of =
    detail::ltsparse_scalar_finder_first_op_of<CharT, true>;
template <typename CharT>
using basic_ltsparse_scalar_finder_first_not_of =
    detail::ltsparse_scalar_finder_first_op_of<CharT, false>;

template <typename CharT>
using basic_default_vector_finder_first_of =
    detail::default_vector_finder_first_op_of<CharT, true>;
template <typename CharT>
using basic_default_vector_finder_first_not_of =
    detail::default_vector_finder_first_op_of<CharT, false>;

template <typename CharT>
using basic_shuffle_vector_finder_first_of =
    detail::shuffle_vector_finder_first_op_of<CharT, true>;
template <typename CharT>
using basic_shuffle_vector_finder_first_not_of =
    detail::shuffle_vector_finder_first_op_of<CharT, false>;

template <typename CharT>
using basic_azmatch_vector_finder_first_of =
    detail::azmatch_vector_finder_first_op_of<CharT, true>;
template <typename CharT>
using basic_azmatch_vector_finder_first_not_of =
    detail::azmatch_vector_finder_first_op_of<CharT, false>;

using stdfind_scalar_finder_first_of =
    basic_stdfind_scalar_finder_first_of<char>;

using default_scalar_finder_first_of =
    basic_default_scalar_finder_first_of<char>;
using default_scalar_finder_first_not_of =
    basic_default_scalar_finder_first_not_of<char>;

using ltindex_scalar_finder_first_of =
    basic_ltindex_scalar_finder_first_of<char>;
using ltindex_scalar_finder_first_not_of =
    basic_ltindex_scalar_finder_first_not_of<char>;

using ltsparse_scalar_finder_first_of =
    basic_ltsparse_scalar_finder_first_of<char>;
using ltsparse_scalar_finder_first_not_of =
    basic_ltsparse_scalar_finder_first_not_of<char>;

using default_vector_finder_first_of =
    basic_default_vector_finder_first_of<char>;
using default_vector_finder_first_not_of =
    basic_default_vector_finder_first_not_of<char>;

using shuffle_vector_finder_first_of =
    basic_shuffle_vector_finder_first_of<char>;
using shuffle_vector_finder_first_not_of =
    basic_shuffle_vector_finder_first_not_of<char>;

using azmatch_vector_finder_first_of =
    basic_azmatch_vector_finder_first_of<char>;
using azmatch_vector_finder_first_not_of =
    basic_azmatch_vector_finder_first_not_of<char>;

/// composite_finder
///
/// A find-first-of finder which composes a vector finder with a scalar finder.
///
/// A vector finder requires a scalar finder for not-implemented-architecture
/// and for near-end-of-input. This combinator producers a finder which uses the
/// vector finder where possible and otherwise falls back to the scalar finder.
template <typename Vector, typename Scalar>
class composite_finder_first_of : private Vector, Scalar {
 private:
  using view = std::span<char const>;

 public:
  constexpr explicit composite_finder_first_of(view const alphabet) noexcept
      : Vector{alphabet}, Scalar{alphabet} {}

  size_t operator()(view const input, size_t const pos = 0) const noexcept {
    auto const& vector = static_cast<Vector const&>(*this);
    auto const& scalar = static_cast<Scalar const&>(*this);
    return vector(scalar, input, pos);
  }
};

} // namespace folly::simd
