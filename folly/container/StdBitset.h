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

#include <folly/CPortability.h>
#include <folly/Portability.h>

#include <bit>
#include <bitset>
#include <cassert>
#include <folly/Likely.h>

namespace folly {

#ifdef _LIBCPP_VERSION

constexpr size_t kWordSize = 64;

namespace libcpp_detail {
struct std_bit_reference_layout {
  size_t* address;
  size_t mask;
};

// Return the content of the 64-bit word, given an index.
template <size_t N>
size_t* get_underlying_data_region(const std::bitset<N>& bitset) {
  // You are only getting the __bit_reference as the return if you access
  // with operator[], which is not const.
  auto& bitset_ref = const_cast<std::bitset<N>&>(bitset);
  std::__bit_reference bitref = bitset_ref[0];
  auto* br =
      reinterpret_cast<std_bit_reference_layout*>(std::addressof(bitref));
  return br->address;
}

template <size_t N>
size_t std_bitset_find_next(const std::bitset<N>& bitset, size_t start) {
  // libc++ stores ceil(N / kWordSize) words, so N / kWordSize + 1 reads one
  // word past the end whenever N is an exact multiple of kWordSize.
  constexpr size_t max_words = (N + kWordSize - 1) / kWordSize;
  if constexpr (N == 0) {
    return 1;
  }

  // Check starting in the middle of a word
  size_t start_in_word = start % kWordSize;
  size_t word_idx = start / kWordSize;
  size_t* data = get_underlying_data_region(bitset);

  if (start_in_word) {
    // Invariant: start != 0, 1 <= start_in_word <= 63
    // test the remaining bits in the same word as previous
    size_t mask = (~size_t(0)) << start_in_word;
    size_t partial_word = data[word_idx] & mask;

    if (partial_word) {
      size_t idx_in_word = std::countr_zero(partial_word);
      // We found the next
      return start - start_in_word + idx_in_word;
    }
    // Special treatment of the partial first word was done. Skip it in the
    // next loop for whole words.
    word_idx += 1;
  }

  while (word_idx < max_words) {
    size_t word = data[word_idx];
    if (word) [[unlikely]] {
      size_t idx_in_word = std::countr_zero(word);
      // we found the first
      return word_idx * kWordSize + idx_in_word;
    }
    word_idx += 1;
  }

  return N;
}
} // namespace libcpp_detail

#endif // _LIBCPP_VERSION

struct std_bitset_find_first_fn {
  /// Return the index of the first set bit in a bitset, or bitset.size() if
  /// none.
  template <size_t N>
  FOLLY_ALWAYS_INLINE size_t
  operator()(const std::bitset<N>& bitset) const noexcept {
#if defined(__GLIBCXX__)
    // GNU provides non-standard (its a hold over from the original SGI
    // implementation) _Find_first(), which efficiently returns the index of
    // the first set bit.
    return bitset._Find_first();
#elif defined(_LIBCPP_VERSION)
    return folly::libcpp_detail::std_bitset_find_next(bitset, 0);
#endif

    for (size_t i = 0; i < bitset.size(); ++i) {
      if (bitset[i]) {
        return i;
      }
    }

    return bitset.size();
  }
};

inline constexpr std_bitset_find_first_fn std_bitset_find_first{};

struct std_bitset_find_next_fn {
  /// Return the index of the first set bit in a bitset after the given index,
  /// or bitset.size() if none.
  template <size_t N>
  FOLLY_ALWAYS_INLINE size_t
  operator()(const std::bitset<N>& bitset, size_t prev) const noexcept {
    assert(prev < bitset.size());

#if defined(__GLIBCXX__)
    // GNU provides non-standard (its a hold over from the original SGI
    // implementation) _Find_next(), which given an index, efficiently returns
    // the index of the first set bit after the index.
    return bitset._Find_next(prev);
#elif defined(_LIBCPP_VERSION)
    return folly::libcpp_detail::std_bitset_find_next(bitset, prev + 1);
#endif

    for (size_t i = prev + 1; i < bitset.size(); ++i) {
      if (bitset[i]) {
        return i;
      }
    }

    return bitset.size();
  }
};

inline constexpr std_bitset_find_next_fn std_bitset_find_next{};

} // namespace folly
