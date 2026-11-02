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

#include <type_traits>
#include <utility>

#include <folly/Portability.h>
#include <folly/Traits.h>

namespace folly {

//  atomic_arch_mm_is_tso_v
//
//  Whether the target architecture guarantees total store order (TSO)
//  for ordinary accesses to coherent write-back memory; currently
//  recognized here for x86 and x86-64. TSO supplies the hardware
//  ordering needed by C++ release stores, acquire loads, and thread
//  fences through acq-rel. Compiler ordering constraints still apply;
//  atomicity and implementation choices can require other instructions.
//
//  Use it to select between multiple correct implementations based on
//  which is expected to perform better on TSO, never between
//  implementations only one of which is correct for the hardware.
inline constexpr bool atomic_arch_mm_is_tso_v = kIsArchX86 || kIsArchAmd64;

namespace detail {

struct atomic_value_type_alias_ {
  template <typename Atomic>
  using apply = typename Atomic::value_type;
};
struct atomic_value_type_load_ {
  template <typename Atomic>
  using apply = decltype(std::declval<Atomic const&>().load());
};

} // namespace detail

//  atomic_value_type_t
//  atomic_value_type
//
//  A trait type alias and type giving the effective value-type of a type which
//  are atomic-like. Either member type alias value_type or the return type of
//  member function load.
template <typename Atomic>
using atomic_value_type_t = typename conditional_t<
    is_detected_v<detail::atomic_value_type_alias_::apply, Atomic>,
    detail::atomic_value_type_alias_,
    detail::atomic_value_type_load_>::template apply<Atomic>;
template <typename Atomic>
struct atomic_value_type {
  using type = atomic_value_type_t<Atomic>;
};

namespace detail {

template <typename Atomic>
constexpr bool atomic_fetch_bitwise_is_native_() {
  using T = std::remove_cv_t<atomic_value_type_t<Atomic>>;
  if constexpr (!std::is_integral_v<T> || std::is_same_v<T, bool>) {
    return false;
  } else if constexpr (kIsArchAArch64Lse || kIsArchWasm) {
    return sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8;
  } else if constexpr (kIsArchRISCVZaamo) {
    if constexpr (sizeof(T) == 4) {
      return true;
    } else if constexpr (sizeof(T) == 8) {
      return kArchRISCVXlen >= 64;
    } else if constexpr (sizeof(T) == 1 || sizeof(T) == 2) {
      return kIsArchRISCVZabha || (kIsClang && kClangVerMajor >= 21);
    }
  }
  return false;
}

} // namespace detail

// Whether value-returning atomic fetch_or/and/xor operations on Atomic lower
// without a compare-exchange loop.
template <typename Atomic>
inline constexpr bool atomic_fetch_bitwise_is_native_v =
    detail::atomic_fetch_bitwise_is_native_<Atomic>();

} // namespace folly
