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

#include <cstring>
#include <iterator>
#include <memory>
#include <type_traits>

namespace folly {

/// uninitialized_copy
///
/// Like std::uninitialized_copy, with a bulk-copy fast path for contiguous
/// iterators over matching, non-volatile types when construction from the
/// source reference is trivial and the destination type is trivially copyable.
///
/// Not all compiler/standard-library combinations optimize
/// std::uninitialized_copy to a bulk copy. This implementation explicitly
/// selects that optimization for eligible ranges.
///
/// Contiguous iterators may be unwrapped to pointers. Side effects of their
/// individual dereference, increment, or decrement operations may be bypassed;
/// callers must not rely on those side effects.
///
/// The destination must provide suitably aligned uninitialized storage, and
/// the source and destination ranges must not overlap. If construction throws,
/// objects already constructed in the destination are destroyed.
///
/// mimic: std::uninitialized_copy, C++98
template <typename InputIt, typename ForwardIt>
ForwardIt uninitialized_copy(InputIt first, InputIt last, ForwardIt result) {
  if constexpr (
      std::contiguous_iterator<InputIt> &&
      std::contiguous_iterator<ForwardIt>) {
    using Source = std::remove_reference_t<std::iter_reference_t<InputIt>>;
    using Destination =
        std::remove_reference_t<std::iter_reference_t<ForwardIt>>;
    if constexpr (
        std::is_same_v<std::remove_cv_t<Source>, Destination> &&
        !std::is_volatile_v<Source> &&
        std::is_trivially_copyable_v<Destination> &&
        std::is_trivially_constructible_v<Destination, Source&>) {
      if (first == last) {
        return result;
      }
      const auto count = last - first;
      std::memcpy(
          std::to_address(result),
          std::to_address(first),
          sizeof(Destination) * count);
      return result + count;
    } else {
      return std::uninitialized_copy(first, last, result);
    }
  } else {
    return std::uninitialized_copy(first, last, result);
  }
}

} // namespace folly
