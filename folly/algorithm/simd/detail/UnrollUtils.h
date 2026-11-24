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

#include <folly/Portability.h>
#include <folly/Traits.h>

#include <array>
#include <type_traits>

namespace folly::simd::detail {

/**
 * UnrollUtils
 *
 * Unfortunately compilers often don't unroll the loops with small
 * fixed number of iterations and/or not unroll them properly.
 *
 * This is a collection of helpers that use templates to do some
 * common unrolled loops.
 */
struct UnrollUtils {
 public:
  /**
   * arrayMap(x, op)
   *
   * Typical "map" from functional languages: apply op for each element,
   * return an array of results.
   */
  template <typename T, std::size_t N, typename Op>
  [[nodiscard]] FOLLY_ALWAYS_INLINE static constexpr auto arrayMap(
      const std::array<T, N>& x, Op op) {
    return arrayMapImpl(x, op, std::make_index_sequence<N>());
  }

  /**
   * arrayReduce(x, op)
   *
   * std::reduce(x.begin(), x.end(), op) but unrolled and orders operations
   * to minimize dependencies.
   *
   * (a + b) + (c + d)
   */
  template <typename T, std::size_t N, typename Op>
  [[nodiscard]] FOLLY_ALWAYS_INLINE static constexpr T arrayReduce(
      const std::array<T, N>& x, Op op) {
    return arrayReduceImpl<0, N>(x, op);
  }

  /**
   * unrollUntil<N>(op)
   *
   * Do operation N times or until it returns true to break.
   * Op accepts index_constant so it can keep track of a step begin executed.
   *
   * Returns whether true if it was interrupted (you can know if the op broke)
   */
  template <std::size_t N, typename Op>
  FOLLY_ALWAYS_INLINE static constexpr bool unrollUntil(Op op) {
    return unrollUntilImpl(op, std::make_index_sequence<N>{});
  }

 private:
  template <typename T, std::size_t N, typename Op, std::size_t... i>
  FOLLY_ALWAYS_INLINE static constexpr auto arrayMapImpl(
      const std::array<T, N>& x, Op op, std::index_sequence<i...> /*unused*/) {
    using U = decltype(op(std::declval<const T&>()));

    FOLLY_PUSH_WARNING
    // This is a very common gcc issue,
    // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=97222 apparently discarding
    // it here is fine and done through out.
    FOLLY_GCC_DISABLE_WARNING("-Wignored-attributes")
    std::array<U, N> res{{op(x[i])...}};
    FOLLY_POP_WARNING
    return res;
  }

  template <
      std::size_t f,
      std::size_t l,
      typename T,
      std::size_t N,
      typename Op>
  FOLLY_ALWAYS_INLINE static constexpr T arrayReduceImpl(
      std::array<T, N> const& x, [[maybe_unused]] Op op) {
    if constexpr (l - f == 1) {
      return x[f];
    } else {
      constexpr std::size_t n = l - f;
      T leftSum = arrayReduceImpl<f, f + n / 2>(x, op);
      T rightSum = arrayReduceImpl<f + n / 2, l>(x, op);
      return op(leftSum, rightSum);
    }
  }

  template <typename Op, std::size_t... i>
  FOLLY_ALWAYS_INLINE static constexpr bool unrollUntilImpl(
      Op op, std::index_sequence<i...> /*unused*/) {
    return (... || op(index_constant<i>{}));
  }
};

} // namespace folly::simd::detail
