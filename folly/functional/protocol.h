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

#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/functional/Invoke.h>
#include <folly/functional/traits.h>

namespace folly {

//  match_empty_function_protocol
//  match_empty_function_protocol_fn
//
//  Evaluates the empty-function protocol for a given value, returning true if
//  the value matches the empty-function protocol and false otherwise.
//
//  A value matches the empty-function protocol when all of the following
//  conditions hold:
//  * its type admits explicit construction with the special value nullptr,
//  * its type admits equality-comparison with the special value nullptr,
//  * it compares-equal-to the special value nullptr.
//
//  In particular, default-constructed values of the following native and
//  standard-library types match the empty-function protocol:
//  * pointer-to-function
//  * pointer-to-member-function
//  * std::function
//  * std::move_only_function
//
//  Likewise, move-constructed-from and move-assigned-from values of the
//  following standard-library types match the empty-function protocol:
//  * std::function
//  * std::move_only_function
//  However, it is unspecified whether self-move-assigned-from values of the
//  above types match the empty-function protocol.
struct match_empty_function_protocol_fn {
 private:
  template <typename T>
  using detect_from_eq_nullptr =
      decltype(static_cast<bool>(static_cast<T const&>(T(nullptr)) == nullptr));

  template <typename T>
  static constexpr bool cx_matches_v =
      require_sizeof<T> && is_detected_v<detect_from_eq_nullptr, T>;

 public:
  template <typename T>
  constexpr bool operator()([[maybe_unused]] T const& t) const noexcept {
    if constexpr (cx_matches_v<T>) {
      return static_cast<bool>(t == nullptr);
    } else {
      return false;
    }
  }
};
inline constexpr match_empty_function_protocol_fn
    match_empty_function_protocol{};

//  ----

//  match_static_lambda_protocol_v
//
//  Evaluates the static-lambda protocol for a given type.
//
//  A value type T matches the static-lambda protocol when all of the following
//  conditions hold:
//  * T is empty
//  * T is trivially-copyable
//
//  In particular, capture-free lambdas match the static-lambda protocol. Static
//  lambdas, introduced in C++23, are capture-free and therefore match the
//  static-lambda protocol as well.
//
//  In particular, certain function-objects in the standard library match the
//  static-lambda protocol:
//  * std::identity
//  * std::less, std::equal_to, std::greater
//  * std::hash
//  * std::default_delete
template <typename F>
static constexpr bool match_static_lambda_protocol_v =
    ( //
        std::is_empty<F>::value && //
        std::is_trivially_copyable_v<F> && //
        true);

//  ----

namespace detail {

template <typename S>
struct match_safely_invocable_as_protocol_impl_ {
  using traits = function_traits<S>;

  using sig_r = typename traits::result;
  static constexpr bool sig_nx = traits::is_nothrow;

  template <typename F>
  using fun_cvref_0 = conditional_t<std::is_reference<F>::value, F, F&>;
  template <typename F>
  using fun_cvref = fun_cvref_0<typename traits::template value_like<F>>;

  template <typename F>
  struct fun_r_ {
    template <typename... A>
    using apply = invoke_result_t<F, A...>;
  };
  template <typename F>
  using fun_r =
      typename traits::template arguments<fun_r_<fun_cvref<F>>::template apply>;

  template <typename F>
  struct fun_inv_ {
    template <typename... A>
    using apply = is_invocable_r<sig_r, F, A...>;
  };
  template <typename F>
  struct fun_inv_nx_ {
    template <typename... A>
    using apply = is_nothrow_invocable_r<sig_r, F, A...>;
  };

  template <typename F>
  using is_invocable_r_ = typename traits::template arguments<
      conditional_t<sig_nx, fun_inv_nx_<F>, fun_inv_<F>>::template apply>;

  template <typename F, typename FCVR = fun_cvref<F>>
  static constexpr bool is_invocable_r_v = std::is_reference<FCVR>::value
      ? is_invocable_r_<F>::value
      : is_invocable_r_<F&>::value && is_invocable_r_<F&&>::value;
};

template <
    typename Fun,
    typename Sig,
    typename Impl = match_safely_invocable_as_protocol_impl_<Sig>,
    typename FunR = typename Impl::template fun_r<Fun>,
    typename SigR = typename Impl::sig_r,
    typename FunQ = typename Impl::template fun_cvref<Fun>,
    bool SigNx = Impl::sig_nx,
    // forbid reference-to-expired-temporary
    typename = std::enable_if_t<
        !std::is_reference<SigR>::value || std::is_reference<FunR>::value>,
    // forbid object slicing: derived maybe-ref to base non-ref conversion
    typename = std::enable_if_t<
        std::is_same<decay_t<SigR>, decay_t<FunR>>::value ||
        std::is_reference<SigR>::value ||
        !std::is_base_of<decay_t<SigR>, decay_t<FunR>>::value>,
    // forbid mismatched cvref
    // forbid pointer with signature cvref
    // forbid noexcept wrapping non-noexcept
    // forbid non-convertible return-type
    // forbid noexcept wrapping non-noexcept return-type conversion
    typename = std::enable_if_t<Impl::template is_invocable_r_v<FunQ>>>
using match_safely_invocable_as_protocol_detect_1_ = void;

template <typename Fun, typename Sig>
using match_safely_invocable_as_protocol_detect_ =
    match_safely_invocable_as_protocol_detect_1_<Fun, Sig>;

} // namespace detail

//  match_safely_invocable_as_protocol_v
//
//  Evaluates the safely-invocable-as protocol for a given type with respect to
//  a given signature. This protocol determines whether a given type honors a
//  set of implied constraints imposed by the given function type (signature).
//
//  A type F matches the safely-invocable-as protocol for a given signature S
//  when all of the following conditions hold:
//  * invocation compatibility
//    * argument-type-list conversions
//    * return-type conversion
//    * cvref compatibility
//    * noexcept compatibility
//  * no reference-to-temporary in return-type conversion
//  * no object slicing in return-type conversion
//
//  The meaning of invocation compatibility above is as per the is-callable-from
//  table in the documentation of std::move_only_function (C++23), extended with
//  volatile qualifications. See:
//    http://eel.is/c++draft/func.wrap.move
template <typename F, typename Sig>
inline constexpr bool match_safely_invocable_as_protocol_v =
    is_detected_v<detail::match_safely_invocable_as_protocol_detect_, F, Sig>;

} // namespace folly
