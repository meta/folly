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

#include <stdexcept>

#include <folly/Likely.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/lang/Exception.h>

namespace folly {

namespace detail {

template <typename C>
inline constexpr bool container_has_reserve_v =
    requires(C& c, typename C::size_type const n) { c.reserve(n); };

} // namespace detail

/**
 * Avoids quadratic behavior that could arise from c.reserve(c.size() + N).
 *
 * May reserve more than N in order to effect geometric growth.  Useful when
 * c.size() is unknown, but more elements must be added by discrete operations.
 * For example: N emplace calls in a loop, or a series of range inserts, the sum
 * of their sizes being N.  Behaves like reserve() if the container is empty.
 */
struct grow_capacity_by_fn {
  template <typename C>
  constexpr void operator()(C& c, typename C::size_type const n) const {
    const size_t sz = c.size();

    if (c.max_size() - sz < n) [[unlikely]] {
      folly::throw_exception<std::length_error>("max_size exceeded");
    }

    constexpr bool has_capacity = requires { c.capacity(); };
    constexpr bool has_bucket_count = requires {
      c.bucket_count();
      c.max_load_factor();
    };
    if constexpr (has_capacity) {
      if (sz + n <= c.capacity()) {
        return;
      }
    } else if constexpr (has_bucket_count) {
      if (sz + n <= c.bucket_count() * c.max_load_factor()) {
        return;
      }
    } else {
      static_assert(folly::always_false<C>, "unexpected container type");
    }

    auto const ra = sz * 2;
    auto const rb = sz + n;
    c.reserve(rb < ra ? ra : rb);
  }
};

inline constexpr grow_capacity_by_fn grow_capacity_by{};

/**
 * Useful when writing generic code that handles containers.
 *
 * Examples:
 *  - std::unordered_map provides reserve(), but std::map does not
 *  - std::vector provides reserve(), but std::deque and std::list do not
 */
struct reserve_if_available_fn {
  template <typename C>
  constexpr auto operator()(C& c, typename C::size_type const n) const
      noexcept(!detail::container_has_reserve_v<C>) {
    constexpr auto match = detail::container_has_reserve_v<C>;
    if constexpr (match) {
      c.reserve(n);
    }
    return std::bool_constant<match>{};
  }
};

inline constexpr reserve_if_available_fn reserve_if_available{};

} // namespace folly
