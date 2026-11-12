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

#include <atomic>
#include <cassert>
#include <cstdint>
#include <tuple>
#include <type_traits>

#include <folly/ConstexprMath.h>
#include <folly/Likely.h>

#ifdef _WIN32
#include <intrin.h>
#endif

namespace folly {

constexpr std::memory_order memory_order_load(
    std::memory_order const order) noexcept {
  constexpr auto relaxed = std::memory_order_relaxed;
  constexpr auto release = std::memory_order_release;
  constexpr auto acquire = std::memory_order_acquire;
  constexpr auto acq_rel = std::memory_order_acq_rel;
  return order == acq_rel ? acquire : order == release ? relaxed : order;
}

constexpr std::memory_order memory_order_store(
    std::memory_order const order) noexcept {
  constexpr auto relaxed = std::memory_order_relaxed;
  constexpr auto release = std::memory_order_release;
  constexpr auto consume = std::memory_order_consume;
  constexpr auto acquire = std::memory_order_acquire;
  constexpr auto acq_rel = std::memory_order_acq_rel;
  // clang-format off
  return
      order == acq_rel ? release :
      order == acquire || order == consume ? relaxed :
      order;
  // clang-format on
}

template <typename T>
class atomic_ref;

namespace detail {

constexpr std::memory_order atomic_compare_exchange_succ(
    bool cond, std::memory_order succ, std::memory_order fail) {
  constexpr auto const relaxed = std::memory_order_relaxed;
  constexpr auto const release = std::memory_order_release;
  constexpr auto const acq_rel = std::memory_order_acq_rel;

  assert(fail != release);
  assert(fail != acq_rel);

  auto const bump = succ == release ? acq_rel : succ;
  auto const high = fail < bump ? bump : fail;
  return !cond || fail == relaxed ? succ : high;
}

//  atomic_compare_exchange_succ
//
//  Return a success order with, conditionally, the failure order mixed in.
//
//  Until C++17, atomic compare-exchange operations require the success order to
//  be at least as strong as the failure order. C++17 drops this requirement. As
//  an implication, were this rule in force, an implementation is free to ignore
//  the explicit failure order and to infer one from the success order.
//
//    https://en.cppreference.com/w/cpp/atomic/atomic/compare_exchange
//
//  The libstdc++ library with assertions enabled enforces this rule, including
//  under C++17, but only until and not since libstdc++12.
//
//    https://github.com/gcc-mirror/gcc/commit/dba1ab212292839572fda60df00965e094a11252
//
//  Clang TSAN ignores the passed failure order and infers failure order from
//  success order in atomic compare-exchange operations, which is broken for
//  cases like success-release/failure-acquire.
//
//    https://github.com/google/sanitizers/issues/970
//
//  All GCC sanitizers statically check the memory-orders passed to compare-
//  exchange operations for correctness and report violations with the warning
//  invalid-memory-model; but, up to the current version (gcc-13), they use the
//  C++14 rule, which is broken for cases like success-relaxed/failure-acquire.
//
//    https://godbolt.org/z/3hjjdGzMv
//
//  Handle all of these cases.
constexpr std::memory_order atomic_compare_exchange_succ(
    std::memory_order succ, std::memory_order fail) {
  constexpr auto const cond = false //
      || (kGlibcxxVer && kGlibcxxVer < 12 && kGlibcxxAssertions) //
      || (kIsSanitizeThread && kIsClang) //
      || (kIsSanitize && !kIsClang && kGnuc) //
      ;
  return atomic_compare_exchange_succ(cond, succ, fail);
}

} // namespace detail

template <template <typename> class Atom, typename T>
bool atomic_compare_exchange_weak_explicit(
    Atom<T>* obj,
    type_t<T>* expected,
    type_t<T> desired,
    std::memory_order succ,
    std::memory_order fail) {
  succ = detail::atomic_compare_exchange_succ(succ, fail);
  return obj->compare_exchange_weak(*expected, desired, succ, fail);
}

template <template <typename> class Atom, typename T>
bool atomic_compare_exchange_strong_explicit(
    Atom<T>* obj,
    type_t<T>* expected,
    type_t<T> desired,
    std::memory_order succ,
    std::memory_order fail) {
  succ = detail::atomic_compare_exchange_succ(succ, fail);
  return obj->compare_exchange_strong(*expected, desired, succ, fail);
}

namespace detail {

// TODO: Remove the non-default implementations when both gcc and clang
// can recognize single bit set/reset patterns and compile them down to locked
// bts and btr instructions.
//
// Currently, at the time of writing it seems like gcc7 and greater can make
// this optimization and clang cannot - https://gcc.godbolt.org/z/Q83rxX

//  the overloads taking no memory order serve types, such as relaxed_atomic,
//  which omit the parameter from their operations; such types never reach the
//  native paths below, which are all specific to std::atomic and to atomic-ref

struct atomic_fetch_set_fallback_fn {
  template <typename Atomic>
  bool operator()(
      Atomic& atomic, std::size_t bit, std::memory_order order) const {
    using Integer = decltype(atomic.load());
    auto mask = Integer(Integer{0b1} << bit);
    return (atomic.fetch_or(mask, order) & mask);
  }
  template <typename Atomic>
  bool operator()(Atomic& atomic, std::size_t bit) const {
    using Integer = decltype(atomic.load());
    auto mask = Integer(Integer{0b1} << bit);
    return (atomic.fetch_or(mask) & mask);
  }
};
inline constexpr atomic_fetch_set_fallback_fn atomic_fetch_set_fallback{};

struct atomic_fetch_reset_fallback_fn {
  template <typename Atomic>
  bool operator()(
      Atomic& atomic, std::size_t bit, std::memory_order order) const {
    using Integer = decltype(atomic.load());
    auto mask = Integer(Integer{0b1} << bit);
    return (atomic.fetch_and(Integer(~mask), order) & mask);
  }
  template <typename Atomic>
  bool operator()(Atomic& atomic, std::size_t bit) const {
    using Integer = decltype(atomic.load());
    auto mask = Integer(Integer{0b1} << bit);
    return (atomic.fetch_and(Integer(~mask)) & mask);
  }
};
inline constexpr atomic_fetch_reset_fallback_fn atomic_fetch_reset_fallback{};

struct atomic_fetch_flip_fallback_fn {
  template <typename Atomic>
  bool operator()(
      Atomic& atomic, std::size_t bit, std::memory_order order) const {
    using Integer = decltype(atomic.load());
    auto mask = Integer(Integer{0b1} << bit);
    return (atomic.fetch_xor(mask, order) & mask);
  }
  template <typename Atomic>
  bool operator()(Atomic& atomic, std::size_t bit) const {
    using Integer = decltype(atomic.load());
    auto mask = Integer(Integer{0b1} << bit);
    return (atomic.fetch_xor(mask) & mask);
  }
};
inline constexpr atomic_fetch_flip_fallback_fn atomic_fetch_flip_fallback{};

#if FOLLY_X64

#if defined(_MSC_VER)

template <typename Integer>
inline bool atomic_fetch_set_native(
    std::atomic<Integer>& atomic, std::size_t bit, std::memory_order order) {
  static_assert(alignof(std::atomic<Integer>) == alignof(Integer));
  static_assert(sizeof(std::atomic<Integer>) == sizeof(Integer));
  assert(atomic.is_lock_free());

  if constexpr (sizeof(Integer) == 4) {
    return _interlockedbittestandset(
        reinterpret_cast<volatile long*>(&atomic), static_cast<long>(bit));
  } else if constexpr (sizeof(Integer) == 8) {
    return _interlockedbittestandset64(
        reinterpret_cast<volatile long long*>(&atomic),
        static_cast<long long>(bit));
  } else {
    assert(sizeof(Integer) != 4 && sizeof(Integer) != 8);
    return atomic_fetch_set_fallback(atomic, bit, order);
  }
}

template <typename Atomic>
inline bool atomic_fetch_set_native(
    Atomic& atomic, std::size_t bit, std::memory_order order) {
  static_assert(!std::is_same<Atomic, std::atomic<std::uint32_t>>{});
  static_assert(!std::is_same<Atomic, std::atomic<std::uint64_t>>{});
  return atomic_fetch_set_fallback(atomic, bit, order);
}

template <typename Integer>
inline bool atomic_fetch_reset_native(
    std::atomic<Integer>& atomic, std::size_t bit, std::memory_order order) {
  static_assert(alignof(std::atomic<Integer>) == alignof(Integer));
  static_assert(sizeof(std::atomic<Integer>) == sizeof(Integer));
  assert(atomic.is_lock_free());

  if constexpr (sizeof(Integer) == 4) {
    return _interlockedbittestandreset(
        reinterpret_cast<volatile long*>(&atomic), static_cast<long>(bit));
  } else if constexpr (sizeof(Integer) == 8) {
    return _interlockedbittestandreset64(
        reinterpret_cast<volatile long long*>(&atomic),
        static_cast<long long>(bit));
  } else {
    assert(sizeof(Integer) != 4 && sizeof(Integer) != 8);
    return atomic_fetch_reset_fallback(atomic, bit, order);
  }
}

template <typename Atomic>
inline bool atomic_fetch_reset_native(
    Atomic& atomic, std::size_t bit, std::memory_order mo) {
  static_assert(!std::is_same<Atomic, std::atomic<std::uint32_t>>{});
  static_assert(!std::is_same<Atomic, std::atomic<std::uint64_t>>{});
  return atomic_fetch_reset_fallback(atomic, bit, mo);
}

template <typename Atomic>
inline bool atomic_fetch_flip_native(
    Atomic& atomic, std::size_t bit, std::memory_order mo) {
  return atomic_fetch_flip_fallback(atomic, bit, mo);
}

#else

enum class atomic_fetch_bit_op_native_instr_mnem { bts, btr, btc };
enum class atomic_fetch_bit_op_native_instr_suff { w, l, q };

#define FOLLY_DETAIL_ATOMIC_BIT_OP_ONE(mnem, suff)                            \
  if constexpr (                                                              \
      Instr == mnem_t::mnem && sizeof(Int) == 1 << (int(suff_t::suff) + 1)) { \
    if (order == ::std::memory_order_relaxed) {                               \
      asm("lock " #mnem #suff " %[bit], %[ptr]"                               \
          : "=@ccc"(out), [ptr] "+m"(*ptr)                                    \
          : [bit] "ri"(bit));                                                 \
    } else {                                                                  \
      asm volatile(                                                           \
          "lock " #mnem #suff " %[bit], (%[ptr])"                             \
          : "=@ccc"(out)                                                      \
          : [bit] "ri"(bit), [ptr] "r"(ptr)                                   \
          : "memory");                                                        \
    }                                                                         \
  }

template <atomic_fetch_bit_op_native_instr_mnem Instr>
struct atomic_fetch_bit_op_native_do_instr_fn {
  template <typename Int>
  FOLLY_ERASE bool operator()(
      Int* ptr, Int bit, std::memory_order order) const {
    using mnem_t = atomic_fetch_bit_op_native_instr_mnem;
    using suff_t = atomic_fetch_bit_op_native_instr_suff;
    bool out = false;
    FOLLY_DETAIL_ATOMIC_BIT_OP_ONE(bts, w)
    FOLLY_DETAIL_ATOMIC_BIT_OP_ONE(bts, l)
    FOLLY_DETAIL_ATOMIC_BIT_OP_ONE(bts, q)
    FOLLY_DETAIL_ATOMIC_BIT_OP_ONE(btr, w)
    FOLLY_DETAIL_ATOMIC_BIT_OP_ONE(btr, l)
    FOLLY_DETAIL_ATOMIC_BIT_OP_ONE(btr, q)
    FOLLY_DETAIL_ATOMIC_BIT_OP_ONE(btc, w)
    FOLLY_DETAIL_ATOMIC_BIT_OP_ONE(btc, l)
    FOLLY_DETAIL_ATOMIC_BIT_OP_ONE(btc, q)
    return out;
  }
};
template <atomic_fetch_bit_op_native_instr_mnem Instr>
inline constexpr atomic_fetch_bit_op_native_do_instr_fn<Instr>
    atomic_fetch_bit_op_native_do_instr{};

static constexpr auto& atomic_fetch_bit_op_native_bts =
    atomic_fetch_bit_op_native_do_instr<
        atomic_fetch_bit_op_native_instr_mnem::bts>;
static constexpr auto& atomic_fetch_bit_op_native_btr =
    atomic_fetch_bit_op_native_do_instr<
        atomic_fetch_bit_op_native_instr_mnem::btr>;
static constexpr auto& atomic_fetch_bit_op_native_btc =
    atomic_fetch_bit_op_native_do_instr<
        atomic_fetch_bit_op_native_instr_mnem::btc>;

#undef FOLLY_DETAIL_ATOMIC_BIT_OP_ONE

template <typename Integer, typename Op, typename Fb>
FOLLY_ERASE bool atomic_fetch_bit_op_native_(
    std::atomic<Integer>& atomic,
    std::size_t bit,
    std::memory_order order,
    Op op,
    Fb fb) {
  constexpr auto atomic_size = sizeof(Integer);
  constexpr auto lo_size = std::size_t(2);
  constexpr auto hi_size = std::size_t(8);
  // some versions of TSAN do not properly instrument the inline assembly
  if (atomic_size > hi_size || folly::kIsSanitize) {
    return fb(atomic, bit, order);
  }
  auto address = reinterpret_cast<std::uintptr_t>(&atomic);
  // there is a minimum word size - if too small, enlarge
  constexpr auto word_size = constexpr_clamp(atomic_size, lo_size, hi_size);
  using word_type = uint_bits_t<word_size * 8>;
  auto adjust = std::size_t(address % lo_size);
  // when the adjustment is not known at compile-time, the extra calculations
  // at run time may cost more than would be saved by using the native op
  if (!__builtin_constant_p(adjust)) {
    return fb(atomic, bit, order);
  }
  address -= adjust;
  bit += 8 * adjust;
  return op(reinterpret_cast<word_type*>(address), word_type(bit), order);
}

#if defined(__cpp_lib_atomic_ref) && __cpp_lib_atomic_ref >= 201806L
template <typename Integer, typename Op, typename Fb>
FOLLY_ERASE bool atomic_fetch_bit_op_native_(
    std::atomic_ref<Integer>& atomic,
    std::size_t bit,
    std::memory_order order,
    Op op,
    Fb fb) {
  if constexpr (!std::atomic_ref<Integer>::is_always_lock_free) {
    return fb(atomic, bit, order);
  }
  auto& ref = **reinterpret_cast<std::atomic<Integer>**>(&atomic);
  return atomic_fetch_bit_op_native_(ref, bit, order, op, fb);
}
#endif

template <typename Integer>
inline bool atomic_fetch_set_native(
    std::atomic<Integer>& atomic, std::size_t bit, std::memory_order order) {
  auto op = atomic_fetch_bit_op_native_bts;
  auto fb = atomic_fetch_set_fallback;
  return atomic_fetch_bit_op_native_(atomic, bit, order, op, fb);
}

template <typename Integer>
inline bool atomic_fetch_set_native(
    atomic_ref<Integer>& atomic, std::size_t bit, std::memory_order order) {
  return atomic_fetch_set_native(atomic.atomic(), bit, order);
}

#if defined(__cpp_lib_atomic_ref) && __cpp_lib_atomic_ref >= 201806L
template <typename Integer>
inline bool atomic_fetch_set_native(
    std::atomic_ref<Integer>& atomic,
    std::size_t bit,
    std::memory_order order) {
  auto op = atomic_fetch_bit_op_native_bts;
  auto fb = atomic_fetch_set_fallback;
  return atomic_fetch_bit_op_native_(atomic, bit, order, op, fb);
}
#endif

template <typename Atomic>
inline bool atomic_fetch_set_native(
    Atomic& atomic, std::size_t bit, std::memory_order order) {
  static_assert(!is_instantiation_of_v<std::atomic, Atomic>);
  return atomic_fetch_set_fallback(atomic, bit, order);
}

template <typename Integer>
inline bool atomic_fetch_reset_native(
    std::atomic<Integer>& atomic, std::size_t bit, std::memory_order order) {
  auto op = atomic_fetch_bit_op_native_btr;
  auto fb = atomic_fetch_reset_fallback;
  return atomic_fetch_bit_op_native_(atomic, bit, order, op, fb);
}

template <typename Integer>
inline bool atomic_fetch_reset_native(
    atomic_ref<Integer>& atomic, std::size_t bit, std::memory_order order) {
  return atomic_fetch_reset_native(atomic.atomic(), bit, order);
}

#if defined(__cpp_lib_atomic_ref) && __cpp_lib_atomic_ref >= 201806L
template <typename Integer>
inline bool atomic_fetch_reset_native(
    std::atomic_ref<Integer>& atomic,
    std::size_t bit,
    std::memory_order order) {
  auto op = atomic_fetch_bit_op_native_btr;
  auto fb = atomic_fetch_reset_fallback;
  return atomic_fetch_bit_op_native_(atomic, bit, order, op, fb);
}
#endif

template <typename Atomic>
bool atomic_fetch_reset_native(
    Atomic& atomic, std::size_t bit, std::memory_order order) {
  static_assert(!is_instantiation_of_v<std::atomic, Atomic>);
  return atomic_fetch_reset_fallback(atomic, bit, order);
}

template <typename Integer>
inline bool atomic_fetch_flip_native(
    std::atomic<Integer>& atomic, std::size_t bit, std::memory_order order) {
  auto op = atomic_fetch_bit_op_native_btc;
  auto fb = atomic_fetch_flip_fallback;
  return atomic_fetch_bit_op_native_(atomic, bit, order, op, fb);
}

template <typename Integer>
inline bool atomic_fetch_flip_native(
    atomic_ref<Integer>& atomic, std::size_t bit, std::memory_order order) {
  return atomic_fetch_flip_native(atomic.atomic(), bit, order);
}

#if defined(__cpp_lib_atomic_ref) && __cpp_lib_atomic_ref >= 201806L
template <typename Integer>
inline bool atomic_fetch_flip_native(
    std::atomic_ref<Integer>& atomic,
    std::size_t bit,
    std::memory_order order) {
  auto op = atomic_fetch_bit_op_native_btc;
  auto fb = atomic_fetch_flip_fallback;
  return atomic_fetch_bit_op_native_(atomic, bit, order, op, fb);
}
#endif

template <typename Atomic>
bool atomic_fetch_flip_native(
    Atomic& atomic, std::size_t bit, std::memory_order order) {
  static_assert(!is_instantiation_of_v<std::atomic, Atomic>);
  return atomic_fetch_flip_fallback(atomic, bit, order);
}

#endif

#else

using atomic_fetch_set_native_fn = detail::atomic_fetch_set_fallback_fn;
inline constexpr atomic_fetch_set_native_fn atomic_fetch_set_native{};

using atomic_fetch_reset_native_fn = detail::atomic_fetch_reset_fallback_fn;
inline constexpr atomic_fetch_reset_native_fn atomic_fetch_reset_native{};

using atomic_fetch_flip_native_fn = detail::atomic_fetch_flip_fallback_fn;
inline constexpr atomic_fetch_flip_native_fn atomic_fetch_flip_native{};

#endif

//  the mo argument is unused for types, such as relaxed_atomic, which omit the
//  parameter from their operations
template <typename Atomic>
atomic_value_type_t<Atomic> atomic_load_(
    Atomic& atomic, std::memory_order const mo) {
  if constexpr (atomic_accepts_memory_order_v<Atomic>) {
    return atomic.load(mo);
  } else {
    return atomic.load();
  }
}

//  whether the bit at the given index is set; the mask matches the one built by
//  the fallback function objects above
template <typename Atomic>
bool atomic_fetch_bit_test_(
    Atomic& atomic, std::size_t bit, std::memory_order mo) {
  using Integer = atomic_value_type_t<Atomic>;
  auto const mask = Integer(Integer{0b1} << bit);
  return bool(atomic_load_(atomic, mo) & mask);
}

template <typename Atomic>
void atomic_fetch_bit_op_check_(Atomic& atomic, std::size_t bit) {
  using Integer = decltype(atomic.load());
  static_assert(std::is_unsigned<Integer>{});
  static_assert(!std::is_const<Atomic>{});
  assert(bit < (sizeof(Integer) * 8));
  (void)bit;
}

} // namespace detail

template <typename Atomic>
bool atomic_fetch_set_fn::operator()(Atomic& atomic, std::size_t bit) const {
  detail::atomic_fetch_bit_op_check_(atomic, bit);
  if constexpr (atomic_accepts_memory_order_v<Atomic>) {
    return detail::atomic_fetch_set_native(
        atomic, bit, std::memory_order_seq_cst);
  } else {
    return detail::atomic_fetch_set_fallback(atomic, bit);
  }
}

template <typename Atomic>
bool atomic_fetch_set_fn::operator()(
    Atomic& atomic, std::size_t bit, std::memory_order mo) const
  requires(atomic_accepts_memory_order_v<Atomic>)
{
  detail::atomic_fetch_bit_op_check_(atomic, bit);
  return detail::atomic_fetch_set_native(atomic, bit, mo);
}

template <typename Atomic>
bool atomic_fetch_reset_fn::operator()(Atomic& atomic, std::size_t bit) const {
  detail::atomic_fetch_bit_op_check_(atomic, bit);
  if constexpr (atomic_accepts_memory_order_v<Atomic>) {
    return detail::atomic_fetch_reset_native(
        atomic, bit, std::memory_order_seq_cst);
  } else {
    return detail::atomic_fetch_reset_fallback(atomic, bit);
  }
}

template <typename Atomic>
bool atomic_fetch_reset_fn::operator()(
    Atomic& atomic, std::size_t bit, std::memory_order mo) const
  requires(atomic_accepts_memory_order_v<Atomic>)
{
  detail::atomic_fetch_bit_op_check_(atomic, bit);
  return detail::atomic_fetch_reset_native(atomic, bit, mo);
}

template <typename Atomic>
bool atomic_fetch_flip_fn::operator()(Atomic& atomic, std::size_t bit) const {
  detail::atomic_fetch_bit_op_check_(atomic, bit);
  if constexpr (atomic_accepts_memory_order_v<Atomic>) {
    return detail::atomic_fetch_flip_native(
        atomic, bit, std::memory_order_seq_cst);
  } else {
    return detail::atomic_fetch_flip_fallback(atomic, bit);
  }
}

template <typename Atomic>
bool atomic_fetch_flip_fn::operator()(
    Atomic& atomic, std::size_t bit, std::memory_order mo) const
  requires(atomic_accepts_memory_order_v<Atomic>)
{
  detail::atomic_fetch_bit_op_check_(atomic, bit);
  return detail::atomic_fetch_flip_native(atomic, bit, mo);
}

template <typename Atomic>
bool atomic_fetch_set_cond_fn::operator()(
    Atomic& atomic, std::size_t bit) const {
  detail::atomic_fetch_bit_op_check_(atomic, bit);
  if (detail::atomic_fetch_bit_test_(atomic, bit, std::memory_order_seq_cst)) {
    return true;
  }
  return atomic_fetch_set(atomic, bit);
}

template <typename Atomic>
bool atomic_fetch_set_cond_fn::operator()(
    Atomic& atomic, std::size_t bit, std::memory_order mo) const
  requires(atomic_accepts_memory_order_v<Atomic>)
{
  detail::atomic_fetch_bit_op_check_(atomic, bit);
  if (detail::atomic_fetch_bit_test_(atomic, bit, memory_order_load(mo))) {
    return true;
  }
  return atomic_fetch_set(atomic, bit, mo);
}

template <typename Atomic>
bool atomic_fetch_reset_cond_fn::operator()(
    Atomic& atomic, std::size_t bit) const {
  detail::atomic_fetch_bit_op_check_(atomic, bit);
  if (!detail::atomic_fetch_bit_test_(atomic, bit, std::memory_order_seq_cst)) {
    return false;
  }
  return atomic_fetch_reset(atomic, bit);
}

template <typename Atomic>
bool atomic_fetch_reset_cond_fn::operator()(
    Atomic& atomic, std::size_t bit, std::memory_order mo) const
  requires(atomic_accepts_memory_order_v<Atomic>)
{
  detail::atomic_fetch_bit_op_check_(atomic, bit);
  if (!detail::atomic_fetch_bit_test_(atomic, bit, memory_order_load(mo))) {
    return false;
  }
  return atomic_fetch_reset(atomic, bit, mo);
}

namespace detail {

//  the mo arguments below are unused for types, such as relaxed_atomic, which
//  omit the parameter from their operations

template <typename Atomic>
bool atomic_compare_exchange_weak_(
    Atomic& atomic,
    atomic_value_type_t<Atomic>& expected,
    atomic_value_type_t<Atomic> const desired,
    std::memory_order const mo) {
  if constexpr (atomic_accepts_memory_order_v<Atomic>) {
    return atomic.compare_exchange_weak(expected, desired, mo);
  } else {
    return atomic.compare_exchange_weak(expected, desired);
  }
}

template <typename Atomic, typename Op>
atomic_value_type_t<Atomic> atomic_fetch_modify_(
    Atomic& atomic, Op op, std::memory_order const mo) {
  auto curr = atomic_load_(atomic, std::memory_order_relaxed);
  auto const& cref = curr;
  while (FOLLY_UNLIKELY(
      !atomic_compare_exchange_weak_(atomic, curr, op(cref), mo))) {
  }
  return curr;
}

} // namespace detail

template <typename Atomic, typename Op>
atomic_value_type_t<Atomic> atomic_fetch_modify_fn::operator()(
    Atomic& atomic, Op op) const {
  return detail::atomic_fetch_modify_(atomic, op, std::memory_order_seq_cst);
}

template <typename Atomic, typename Op>
atomic_value_type_t<Atomic> atomic_fetch_modify_fn::operator()(
    Atomic& atomic, Op op, std::memory_order const mo) const
  requires(atomic_accepts_memory_order_v<Atomic>)
{
  return detail::atomic_fetch_modify_(atomic, op, mo);
}

namespace detail {

template <typename Atomic>
inline constexpr bool has_atomic_fetch_min_member_v =
    requires(Atomic& atomic, std::memory_order const mo) {
      atomic.fetch_min(atomic_value_type_t<Atomic>{}, mo);
    } || requires(Atomic& atomic) {
      atomic.fetch_min(atomic_value_type_t<Atomic>{});
    };

template <typename Atomic>
inline constexpr bool has_atomic_fetch_max_member_v =
    requires(Atomic& atomic, std::memory_order const mo) {
      atomic.fetch_max(atomic_value_type_t<Atomic>{}, mo);
    } || requires(Atomic& atomic) {
      atomic.fetch_max(atomic_value_type_t<Atomic>{});
    };

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_min_(
    Atomic& atomic,
    atomic_value_type_t<Atomic> const value,
    std::memory_order const mo) {
  using value_type = atomic_value_type_t<Atomic>;
  if constexpr (requires { atomic.fetch_min(value_type{}, mo); }) {
    return atomic.fetch_min(value, mo);
  } else if constexpr (requires { atomic.fetch_min(value_type{}); }) {
    return atomic.fetch_min(value);
  } else {
    auto const op = [=](auto const& v) { return value < v ? value : v; };
    return atomic_fetch_modify_(atomic, op, mo);
  }
}

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_max_(
    Atomic& atomic,
    atomic_value_type_t<Atomic> const value,
    std::memory_order const mo) {
  using value_type = atomic_value_type_t<Atomic>;
  if constexpr (requires { atomic.fetch_max(value_type{}, mo); }) {
    return atomic.fetch_max(value, mo);
  } else if constexpr (requires { atomic.fetch_max(value_type{}); }) {
    return atomic.fetch_max(value);
  } else {
    auto const op = [=](auto const& v) { return v < value ? value : v; };
    return atomic_fetch_modify_(atomic, op, mo);
  }
}

//  the guards below must mirror the comparators above exactly, and must use
//  only operator<, so that the conditional forms elide the store on precisely
//  the inputs for which the unconditional forms would store the same value
//
//  the trial load takes only the load part, since it never stores; the delegate
//  takes the order in full, since it is itself a read-modify-write and its own
//  load component, which yields the returned value, requires the load part
//
//  the branch hints optimize for the converged case, in which no store is
//  necessary; where there is no member the c/x loop re-tests the guard on each
//  retry, so a value converged concurrently mid-loop also elides the store

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_min_cond_(
    Atomic& atomic,
    atomic_value_type_t<Atomic> const value,
    std::memory_order const mo) {
  auto curr = atomic_load_(atomic, memory_order_load(mo));
  if constexpr (has_atomic_fetch_min_member_v<Atomic>) {
    return FOLLY_UNLIKELY(value < curr) //
        ? atomic_fetch_min_(atomic, value, mo)
        : curr;
  } else {
    while (FOLLY_UNLIKELY(value < curr)) {
      if (atomic_compare_exchange_weak_(atomic, curr, value, mo)) {
        break;
      }
    }
    return curr;
  }
}

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_max_cond_(
    Atomic& atomic,
    atomic_value_type_t<Atomic> const value,
    std::memory_order const mo) {
  auto curr = atomic_load_(atomic, memory_order_load(mo));
  if constexpr (has_atomic_fetch_max_member_v<Atomic>) {
    return FOLLY_UNLIKELY(curr < value) //
        ? atomic_fetch_max_(atomic, value, mo)
        : curr;
  } else {
    while (FOLLY_UNLIKELY(curr < value)) {
      if (atomic_compare_exchange_weak_(atomic, curr, value, mo)) {
        break;
      }
    }
    return curr;
  }
}

} // namespace detail

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_min_fn::operator()(
    Atomic& atomic, atomic_value_type_t<Atomic> const value) const {
  return detail::atomic_fetch_min_(atomic, value, std::memory_order_seq_cst);
}

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_min_fn::operator()(
    Atomic& atomic,
    atomic_value_type_t<Atomic> const value,
    std::memory_order const mo) const
  requires(atomic_accepts_memory_order_v<Atomic>)
{
  return detail::atomic_fetch_min_(atomic, value, mo);
}

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_max_fn::operator()(
    Atomic& atomic, atomic_value_type_t<Atomic> const value) const {
  return detail::atomic_fetch_max_(atomic, value, std::memory_order_seq_cst);
}

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_max_fn::operator()(
    Atomic& atomic,
    atomic_value_type_t<Atomic> const value,
    std::memory_order const mo) const
  requires(atomic_accepts_memory_order_v<Atomic>)
{
  return detail::atomic_fetch_max_(atomic, value, mo);
}

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_min_cond_fn::operator()(
    Atomic& atomic, atomic_value_type_t<Atomic> const value) const {
  return detail::atomic_fetch_min_cond_(
      atomic, value, std::memory_order_seq_cst);
}

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_min_cond_fn::operator()(
    Atomic& atomic,
    atomic_value_type_t<Atomic> const value,
    std::memory_order const mo) const
  requires(atomic_accepts_memory_order_v<Atomic>)
{
  return detail::atomic_fetch_min_cond_(atomic, value, mo);
}

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_max_cond_fn::operator()(
    Atomic& atomic, atomic_value_type_t<Atomic> const value) const {
  return detail::atomic_fetch_max_cond_(
      atomic, value, std::memory_order_seq_cst);
}

template <typename Atomic>
atomic_value_type_t<Atomic> atomic_fetch_max_cond_fn::operator()(
    Atomic& atomic,
    atomic_value_type_t<Atomic> const value,
    std::memory_order const mo) const
  requires(atomic_accepts_memory_order_v<Atomic>)
{
  return detail::atomic_fetch_max_cond_(atomic, value, mo);
}

} // namespace folly
