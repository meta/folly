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

#include <array>
#include <functional>
#include <type_traits>

#include <folly/CPortability.h>
#include <folly/Preprocessor.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/lang/CustomizationPoint.h>

#define FOLLY_DETAIL_FORWARD_REF(a) static_cast<decltype(a)&&>(a)

namespace folly {

namespace invoke_detail {

//  ok_one_
//
//  A quoted-metafunction which, when applied to type T, enforces that T is a
//  complete type, (possibly cv-qualified) void, or an array of unknown bound.
//  Substitutes as void if that holds; otherwise fails a static-assert.
struct ok_one_ {
  template <typename T>
  static constexpr bool pass_v =
      ( //
          std::is_void_v<T> || //
          std::is_reference_v<T> || //
          std::is_function_v<T> || //
          std::is_unbounded_array_v<T> || //
          false);

  // note: void return type with no function body to enforce that, in the
  // typical case of complete non-function types, to minimize the quantity of
  // evaluations and instantiations
  template <typename T, std::size_t = sizeof(T)>
  static void test(int);

  // note: auto return type with no trailing return type to force the
  // instantiation of the function and, therefore, to force the
  // evaluation of she static-assert
  template <typename T>
  static auto test(...) {
    static_assert(pass_v<T>, "must be complete, cv-void, or unbounded-array");
    return;
  }

  template <typename T>
  using apply = decltype(test<T>(0));
};

//  ok_
//
//  Enforce that each A... is a complete type, (possibly cv-qualified) void, or
//  an array of unknown bound. Substitutes as T if that holds; otherwise fails a
//  static-assert.
//
//  The reason to fail a static-assert and not, say, to fail to substitute is
//  to force application to incomplete types to fail the compile rather than to
//  allow the compile to succumb to ODR violation. The failing static-assert is
//  a diagnosis of undefined behavior.
//
//  See:
//    https://en.cppreference.com/w/cpp/types/is_invocable
//    https://github.com/gcc-mirror/gcc/blob/releases/gcc-13.2.0/libstdc%2B%2B-v3/include/std/type_traits#L272-L287
template <typename T, typename... A>
using ok_ = type_t<T, ok_one_::apply<A>...>;

template <typename F>
struct traits {
  template <typename... A>
  using result = decltype( //
      FOLLY_DECLVAL(F&&)(FOLLY_DECLVAL(A&&)...));
#if defined(_MSC_VER) && defined(__NVCC__)
  template <typename... A>
  using nothrow_t = std::bool_constant<noexcept( //
      FOLLY_DECLVAL(F&&)(FOLLY_DECLVAL(A&&)...))>;
#else
  template <typename... A>
  static constexpr bool nothrow_v = noexcept( //
      FOLLY_DECLVAL(F&&)(FOLLY_DECLVAL(A&&)...));
#endif
};
template <typename P>
struct traits_member_ptr {
  template <typename... A>
  using result = decltype( //
      std::mem_fn(FOLLY_DECLVAL(P))(FOLLY_DECLVAL(A&&)...));
#if defined(_MSC_VER) && defined(__NVCC__)
  template <typename... A>
  using nothrow_t = std::bool_constant<noexcept( //
      std::mem_fn(FOLLY_DECLVAL(P))(FOLLY_DECLVAL(A&&)...))>;
#else
  template <typename... A>
  static constexpr bool nothrow_v = noexcept( //
      std::mem_fn(FOLLY_DECLVAL(P))(FOLLY_DECLVAL(A&&)...));
#endif
};
template <typename M, typename C>
struct traits<M C::*> : traits_member_ptr<M C::*> {};
template <typename M, typename C>
struct traits<M C::* const> : traits_member_ptr<M C::*> {};
template <typename M, typename C>
struct traits<M C::*&> : traits_member_ptr<M C::*> {};
template <typename M, typename C>
struct traits<M C::* const&> : traits_member_ptr<M C::*> {};
template <typename M, typename C>
struct traits<M C::*&&> : traits_member_ptr<M C::*> {};
template <typename M, typename C>
struct traits<M C::* const&&> : traits_member_ptr<M C::*> {};

#if defined(_MSC_VER) && defined(__NVCC__)
template <typename P, typename... A>
using is_nothrow = traits<P>::template nothrow_t<A...>;
#else
template <typename P, typename... A>
using is_nothrow = std::bool_constant<traits<P>::template nothrow_v<A...>>;
#endif

template <typename R>
void conv_r_(R, decay_t<R>* = nullptr) noexcept;

template <bool NX, typename R, typename FR>
inline constexpr bool conv_r_v_ = std::is_void_v<R> || requires {
  conv_r_<R>(FOLLY_DECLVAL(FR));
  requires !NX || noexcept(conv_r_<R>(FOLLY_DECLVAL(FR)));
};

//  adapted from: http://en.cppreference.com/w/cpp/types/result_of, CC-BY-SA

template <typename F, typename... A>
using invoke_result_t = typename traits<F>::template result<A...>;

template <typename F, typename... A>
concept invocable_ = requires { typename invoke_result_t<F, A...>; };

template <typename F, typename... A>
struct invoke_result {};

template <typename F, typename... A>
  requires invocable_<F, A...>
struct invoke_result<F, A...> {
  using type = invoke_result_t<F, A...>;
};

template <typename F, typename... A>
inline constexpr bool is_invocable_v = ok_<bool, F, A...>{false};

template <typename F, typename... A>
  requires invocable_<F, A...>
inline constexpr bool is_invocable_v<F, A...> = true;

template <typename R, typename F, typename... A>
inline constexpr bool is_invocable_r_v = ok_<bool, R, F, A...>{false};

template <typename R, typename F, typename... A>
  requires invocable_<F, A...>
inline constexpr bool is_invocable_r_v<R, F, A...> =
    conv_r_v_<false, R, invoke_result_t<F, A...>>;

template <typename F, typename... A>
inline constexpr bool is_nothrow_invocable_v = ok_<bool, F, A...>{false};

template <typename F, typename... A>
  requires invocable_<F, A...>
inline constexpr bool is_nothrow_invocable_v<F, A...> =
    is_nothrow<F, A...>::value;

template <typename R, typename F, typename... A>
inline constexpr bool is_nothrow_invocable_r_v = ok_<bool, R, F, A...>{false};

template <typename R, typename F, typename... A>
  requires invocable_<F, A...>
inline constexpr bool is_nothrow_invocable_r_v<R, F, A...> =
    is_nothrow<F, A...>::value && conv_r_v_<true, R, invoke_result_t<F, A...>>;

} // namespace invoke_detail

//  mimic: std::invoke_result, C++17
template <typename F, typename... A>
using invoke_result = invoke_detail::invoke_result<F, A...>;

//  mimic: std::invoke_result_t, C++17
using invoke_detail::invoke_result_t;

//  mimic: std::is_invocable, C++17
template <typename F, typename... A>
struct is_invocable
    : std::bool_constant<invoke_detail::is_invocable_v<F, A...>> {};

//  mimic: std::is_invocable_r_v, C++17
template <typename R, typename F, typename... A>
inline constexpr bool is_invocable_r_v =
    invoke_detail::is_invocable_r_v<R, F, A...>;

//  mimic: std::is_invocable_r, C++17
template <typename R, typename F, typename... A>
struct is_invocable_r : std::bool_constant<is_invocable_r_v<R, F, A...>> {};

//  mimic: std::is_nothrow_invocable_v, C++17
template <typename F, typename... A>
inline constexpr bool is_nothrow_invocable_v =
    invoke_detail::is_nothrow_invocable_v<F, A...>;

//  mimic: std::is_nothrow_invocable, C++17
template <typename F, typename... A>
struct is_nothrow_invocable
    : std::bool_constant<is_nothrow_invocable_v<F, A...>> {};

//  mimic: std::is_nothrow_invocable_r_v, C++17
template <typename R, typename F, typename... Args>
inline constexpr bool is_nothrow_invocable_r_v =
    invoke_detail::is_nothrow_invocable_r_v<R, F, Args...>;

//  mimic: std::is_nothrow_invocable_r, C++17
template <typename R, typename F, typename... A>
struct is_nothrow_invocable_r
    : std::bool_constant<is_nothrow_invocable_r_v<R, F, A...>> {};

} // namespace folly

namespace folly {

namespace detail {

struct invoke_private_overload;

template <bool, typename I>
struct invoke_traits_base_ {};
template <typename I>
struct invoke_traits_base_<true, I> {
  inline static constexpr I invoke{};
};
template <typename I>
using invoke_traits_base =
    invoke_traits_base_<is_constexpr_default_constructible_v<I>, I>;

} // namespace detail

//  invoke_traits
//
//  A traits container struct with the following member types, type aliases, and
//  variables:
//
//  * invoke_result
//  * invoke_result_t
//  * is_invocable
//  * is_invocable_v
//  * is_invocable_r
//  * is_invocable_r_v
//  * is_nothrow_invocable
//  * is_nothrow_invocable_v
//  * is_nothrow_invocable_r
//  * is_nothrow_invocable_r_v
//
//  These members have behavior matching the behavior of C++17's corresponding
//  invocation traits types, type aliases, and variables, but using invoke_type
//  as the invocable argument passed to the usual nivocation traits.
//
//  The traits container struct also has a member type alias:
//
//  * invoke_type
//
//  And an invocable variable as a default-constructed instance of invoke_type,
//  if the latter is constexpr default-constructible:
//
//  * invoke
template <typename I>
struct invoke_traits : detail::invoke_traits_base<I> {
 public:
  using invoke_type = I;

  //  If invoke_type is constexpr default-constructible:
  //
  //    inline static constexpr invoke_type invoke{};

  template <typename... A>
  using invoke_result = invoke_detail::invoke_result<I, A...>;
  template <typename... A>
  using invoke_result_t = invoke_detail::invoke_result_t<I, A...>;
  template <typename... A>
  inline static constexpr bool is_invocable_v =
      invoke_detail::is_invocable_v<I, A...>;
  template <typename... A>
  struct is_invocable //
      : std::bool_constant<invoke_detail::is_invocable_v<I, A...>> {};
  template <typename R, typename... A>
  inline static constexpr bool is_invocable_r_v =
      invoke_detail::is_invocable_r_v<R, I, A...>;
  template <typename R, typename... A>
  struct is_invocable_r //
      : std::bool_constant< //
            invoke_detail::is_invocable_r_v<R, I, A...>> {};
  template <typename... A>
  inline static constexpr bool is_nothrow_invocable_v =
      invoke_detail::is_nothrow_invocable_v<I, A...>;
  template <typename... A>
  struct is_nothrow_invocable //
      : std::bool_constant<invoke_detail::is_nothrow_invocable_v<I, A...>> {};
  template <typename R, typename... A>
  inline static constexpr bool is_nothrow_invocable_r_v =
      invoke_detail::is_nothrow_invocable_r_v<R, I, A...>;
  template <typename R, typename... A>
  struct is_nothrow_invocable_r //
      : std::bool_constant<
            invoke_detail::is_nothrow_invocable_r_v<R, I, A...>> {};
};

//  invoke_first_match
//
//  A composite invoker which delegates to the first invoker parameter matching
//  the call.
//
//  Example:
//
//      FOLLY_CREATE_QUAL_INVOKER(invoke_x_fn, x);
//      FOLLY_CREATE_QUAL_INVOKER(invoke_y_fn, y);
//
//      using invoke_x_or_y_fn = invoke_first_match<invoke_x_fn, invoke_y_fn>;
//      inline constexpr invoke_x_or_y_fn invoke_x_or_y;
//
//      void go(int a, int b) { invoke_x_or_y(a, b); }
//
//  In this example, go(...) will delegate to x(...) if it exists and is a match
//  for the arguments, or otherwise will delegate to y(...).
template <typename... Invoker>
struct invoke_first_match : private Invoker... {
 private:
  using iseq = std::index_sequence_for<Invoker...>;
  template <size_t Idx>
  using at = type_pack_element_t<Idx, Invoker...>;
  template <size_t... Idx, typename... A>
  static constexpr size_t first_(std::index_sequence<Idx...>, tag_t<A...>) {
    constexpr std::array<bool, sizeof...(Idx) + 1> r = {
        {invoke_detail::is_invocable_v<at<Idx> const&, A...>..., false}};
    for (size_t i = 0; i < sizeof...(Invoker); ++i) {
      if (r[i]) {
        return i;
      }
    }
    return sizeof...(Invoker);
  }
  template <typename... A>
  static constexpr size_t first = first_(iseq{}, tag<A...>);

 public:
  template <typename... A, typename Inv = at<first<A...>>>
  FOLLY_ERASE constexpr auto operator()(A&&... a) const
      noexcept(is_nothrow_invocable_v<Inv const&, A...>)
          -> invoke_result_t<Inv const&, A...> {
    return static_cast<Inv const&>(*this)(static_cast<A&&>(a)...);
  }
};

} // namespace folly

#define FOLLY_DETAIL_CREATE_FREE_INVOKE_TRAITS_USING_1(funcname, ns) \
  using ns::funcname;

#define FOLLY_DETAIL_CREATE_FREE_INVOKE_TRAITS_USING(_, funcname, ...) \
  FOLLY_PP_FOR_EACH_WITH(                                              \
      FOLLY_DETAIL_CREATE_FREE_INVOKE_TRAITS_USING_1, funcname, __VA_ARGS__)

/***
 *  FOLLY_CREATE_FREE_INVOKER
 *
 *  Used to create an invoker type bound to a specific free-invocable name.
 *
 *  Example:
 *
 *    FOLLY_CREATE_FREE_INVOKER(foo_invoker, foo);
 *
 *  The type `foo_invoker` is generated in the current namespace and may be used
 *  as follows:
 *
 *    namespace Deep {
 *    struct CanFoo {};
 *    int foo(CanFoo const&, Bar&) { return 1; }
 *    int foo(CanFoo&&, Car&&) noexcept { return 2; }
 *    }
 *
 *    using traits = folly::invoke_traits<foo_invoker>;
 *
 *    traits::invoke(Deep::CanFoo{}, Car{}) // 2
 *
 *    traits::invoke_result<Deep::CanFoo, Bar&> // has member
 *    traits::invoke_result_t<Deep::CanFoo, Bar&> // int
 *    traits::invoke_result<Deep::CanFoo, Bar&&> // empty
 *    traits::invoke_result_t<Deep::CanFoo, Bar&&> // error
 *
 *    traits::is_invocable_v<CanFoo, Bar&> // true
 *    traits::is_invocable_v<CanFoo, Bar&&> // false
 *
 *    traits::is_invocable_r_v<int, CanFoo, Bar&> // true
 *    traits::is_invocable_r_v<char*, CanFoo, Bar&> // false
 *
 *    traits::is_nothrow_invocable_v<CanFoo, Bar&> // false
 *    traits::is_nothrow_invocable_v<CanFoo, Car&&> // true
 *
 *    traits::is_nothrow_invocable_v<int, CanFoo, Bar&> // false
 *    traits::is_nothrow_invocable_v<char*, CanFoo, Bar&> // false
 *    traits::is_nothrow_invocable_v<int, CanFoo, Car&&> // true
 *    traits::is_nothrow_invocable_v<char*, CanFoo, Car&&> // false
 *
 *  When a name has one or more primary definition in a fixed set of namespaces
 *  and alternate definitions in the namespaces of its arguments, the primary
 *  definitions may automatically be found as follows:
 *
 *    FOLLY_CREATE_FREE_INVOKER(swap_invoker, swap, std);
 *
 *  In this case, `swap_invoke_traits::invoke(int&, int&)` will use the primary
 *  definition found in `namespace std` relative to the current namespace, which
 *  may be equivalent to `namespace ::std`. In contrast:
 *
 *    namespace Deep {
 *    struct HasData {};
 *    void swap(HasData&, HasData&) { throw 7; }
 *    }
 *
 *    using traits = invoke_traits<swap_invoker>;
 *
 *    HasData a, b;
 *    traits::invoke(a, b); // throw 7
 *
 *  The namespace list takes at most 15 entries. An empty entry means the global
 *  namespace.
 */
#define FOLLY_CREATE_FREE_INVOKER(classname, funcname, ...)                    \
  namespace classname##__folly_detail_invoke_ns {                              \
    [[maybe_unused]] void funcname(::folly::detail::invoke_private_overload&); \
    FOLLY_DETAIL_CREATE_FREE_INVOKE_TRAITS_USING(_, funcname, __VA_ARGS__)     \
    struct __folly_detail_invoke_obj {                                         \
      template <typename... Args>                                              \
      [[maybe_unused]] FOLLY_ERASE_HACK_GCC constexpr auto operator()(         \
          Args&&... args) const                                                \
          noexcept(noexcept(funcname(static_cast<Args&&>(args)...)))           \
              -> decltype(funcname(static_cast<Args&&>(args)...)) {            \
        return funcname(static_cast<Args&&>(args)...);                         \
      }                                                                        \
    };                                                                         \
  }                                                                            \
  struct classname                                                             \
      : classname##__folly_detail_invoke_ns::__folly_detail_invoke_obj {}

/***
 *  FOLLY_CREATE_FREE_INVOKER_SUITE
 *
 *  Used to create an invoker type and associated variable bound to a specific
 *  free-invocable name. The invoker variable is named like the free-invocable
 *  name and the invoker type is named with a suffix of _fn.
 *
 *  See FOLLY_CREATE_FREE_INVOKER, including its limit of 15 namespaces.
 */
#define FOLLY_CREATE_FREE_INVOKER_SUITE(funcname, ...)             \
  FOLLY_CREATE_FREE_INVOKER(funcname##_fn, funcname, __VA_ARGS__); \
  [[maybe_unused]] inline constexpr funcname##_fn funcname {}

/***
 *  FOLLY_CREATE_QUAL_INVOKER
 *
 *  Used to create an invoker type bound to a specific free-invocable qualified
 *  name. It is permitted that the qualification be empty and that the name be
 *  unqualified in practice. This differs from FOLLY_CREATE_FREE_INVOKER in that
 *  it is required that the name be in scope and that it is not possible to
 *  provide a list of namespaces in which to look up the name..
 */
#define FOLLY_CREATE_QUAL_INVOKER(classname, funcpath)                        \
  struct classname {                                                          \
    template <typename... A>                                                  \
    [[maybe_unused]] FOLLY_ERASE_HACK_GCC constexpr auto operator()(A&&... a) \
        const FOLLY_DETAIL_FORWARD_BODY(funcpath(static_cast<A&&>(a)...))     \
  }

/***
 *  FOLLY_CREATE_QUAL_INVOKER_SUITE
 *
 *  Used to create an invoker type and associated variable bound to a specific
 *  free-invocable qualified name.
 *
 *  See FOLLY_CREATE_QUAL_INVOKER.
 */
#define FOLLY_CREATE_QUAL_INVOKER_SUITE(name, funcpath) \
  FOLLY_CREATE_QUAL_INVOKER(name##_fn, funcpath);       \
  [[maybe_unused]] inline constexpr name##_fn name {}

/***
 *  FOLLY_INVOKE_QUAL
 *
 *  An invoker expression resulting in an invocable which, when invoked, invokes
 *  the free-invocable qualified name with the given arguments.
 */
#define FOLLY_INVOKE_QUAL(funcpath)                                  \
  [](auto&&... __folly_param_a) constexpr FOLLY_DETAIL_FORWARD_BODY( \
      funcpath(FOLLY_DETAIL_FORWARD_REF(__folly_param_a)...))

/***
 *  FOLLY_CREATE_MEMBER_INVOKER
 *
 *  Used to create an invoker type bound to a specific member-invocable name.
 *
 *  Example:
 *
 *    FOLLY_CREATE_MEMBER_INVOKER(foo_invoker, foo);
 *
 *  The type `foo_invoker` is generated in the current namespace and may be used
 *  as follows:
 *
 *    struct CanFoo {
 *      int foo(Bar&) { return 1; }
 *      int foo(Car&&) noexcept { return 2; }
 *    };
 *
 *    using traits = folly::invoke_traits<foo_invoker>;
 *
 *    traits::invoke(CanFoo{}, Car{}) // 2
 *
 *    traits::invoke_result<CanFoo, Bar&> // has member
 *    traits::invoke_result_t<CanFoo, Bar&> // int
 *    traits::invoke_result<CanFoo, Bar&&> // empty
 *    traits::invoke_result_t<CanFoo, Bar&&> // error
 *
 *    traits::is_invocable_v<CanFoo, Bar&> // true
 *    traits::is_invocable_v<CanFoo, Bar&&> // false
 *
 *    traits::is_invocable_r_v<int, CanFoo, Bar&> // true
 *    traits::is_invocable_r_v<char*, CanFoo, Bar&> // false
 *
 *    traits::is_nothrow_invocable_v<CanFoo, Bar&> // false
 *    traits::is_nothrow_invocable_v<CanFoo, Car&&> // true
 *
 *    traits::is_nothrow_invocable_v<int, CanFoo, Bar&> // false
 *    traits::is_nothrow_invocable_v<char*, CanFoo, Bar&> // false
 *    traits::is_nothrow_invocable_v<int, CanFoo, Car&&> // true
 *    traits::is_nothrow_invocable_v<char*, CanFoo, Car&&> // false
 */
#define FOLLY_CREATE_MEMBER_INVOKER(classname, membername)                 \
  struct classname {                                                       \
    template <typename O, typename... Args>                                \
    [[maybe_unused]] FOLLY_ERASE_HACK_GCC constexpr auto                   \
    operator()(O&& o, Args&&... args) const noexcept(noexcept(             \
        static_cast<O&&>(o).membername(static_cast<Args&&>(args)...)))     \
        -> decltype(static_cast<O&&>(o).membername(                        \
            static_cast<Args&&>(args)...)) {                               \
      return static_cast<O&&>(o).membername(static_cast<Args&&>(args)...); \
    }                                                                      \
  }

/***
 *  FOLLY_CREATE_MEMBER_INVOKER_SUITE
 *
 *  Used to create an invoker type and associated variable bound to a specific
 *  member-invocable name. The invoker variable is named like the member-
 *  invocable  name and the invoker type is named with a suffix of _fn.
 *
 *  See FOLLY_CREATE_MEMBER_INVOKER.
 */
#define FOLLY_CREATE_MEMBER_INVOKER_SUITE(membername)       \
  FOLLY_CREATE_MEMBER_INVOKER(membername##_fn, membername); \
  [[maybe_unused]] inline constexpr membername##_fn membername {}

namespace folly {
// Power users can call `is_instantiation_of<invoke_member_wrapper_fn, T>`
// to ascertain that a callable was made via `FOLLY_INVOKE_MEMBER`.  This
// can be important e.g. when you want to be sure that the first argument
// of the callable becomes the implicit object parameter of the class.
template <typename F>
struct invoke_member_wrapper_fn : private F {
  template <typename G, typename = decltype(F(FOLLY_DECLVAL(G&&)))>
  constexpr explicit invoke_member_wrapper_fn(G&& f) noexcept(
      noexcept(F(static_cast<G&&>(f))))
      : F(static_cast<G&&>(f)) {}
  using F::operator();
};
template <typename F>
invoke_member_wrapper_fn(F) -> invoke_member_wrapper_fn<F>;
} // namespace folly

/***
 *  FOLLY_INVOKE_MEMBER
 *
 *  An invoker expression resulting in an invocable which, when invoked, invokes
 *  the member on the object with the given arguments.
 *
 *  Example:
 *
 *    FOLLY_INVOKE_MEMBER(find)(map, key)
 *
 *  Equivalent to:
 *
 *    map.find(key)
 *
 *  But also equivalent to:
 *
 *    std::invoke(FOLLY_INVOKE_MEMBER(find), map, key)
 *
 *  As an implementation detail, the resulting callable uses a lambda.  This
 *  has two observable consequences.
 *  * Since C++17 only, lambda invocations may be marked constexpr.
 *  * Since C++20 only, lambda definitions may appear in an unevaluated context,
 *    namely, in an operand to decltype, noexcept, sizeof, or typeid.
 */
#define FOLLY_INVOKE_MEMBER(membername)                                                          \
  ::folly::invoke_member_wrapper_fn(                                                             \
      [](auto&& __folly_param_o, auto&&... __folly_param_a) constexpr FOLLY_DETAIL_FORWARD_BODY( \
          FOLLY_DETAIL_FORWARD_REF(__folly_param_o)                                              \
              .membername(FOLLY_DETAIL_FORWARD_REF(__folly_param_a)...)))

/***
 *  FOLLY_CREATE_MEMBER_ACCESSOR
 *
 *  Used to create an accessor type bound to a specific data-member name,
 *  providing access to that data-member.
 */
#define FOLLY_CREATE_MEMBER_ACCESSOR(classname, membername)            \
  struct classname {                                                   \
    template <typename T>                                              \
    [[maybe_unused]] FOLLY_ERASE_HACK_GCC constexpr auto&& operator()( \
        T&& val) const noexcept {                                      \
      return static_cast<T&&>(val).membername;                         \
    }                                                                  \
  }

/***
 *  FOLLY_CREATE_MEMBER_ACCESSOR_SUITE
 *
 *  Used to create an accessor type and associated variable bound to a specific
 *  data-member name, providing access to that data-member. The accessor
 *  variable is named like the member name and the accessor type is named with a
 *  suffix of _fn.
 */
#define FOLLY_CREATE_MEMBER_ACCESSOR_SUITE(membername)       \
  FOLLY_CREATE_MEMBER_ACCESSOR(membername##_fn, membername); \
  [[maybe_unused]] inline constexpr membername##_fn membername {}

namespace folly {

namespace detail_tag_invoke_fn {

void tag_invoke();

struct tag_invoke_fn {
  template <typename Tag, typename... Args>
  constexpr auto operator()(Tag tag, Args&&... args) const noexcept(noexcept(
      tag_invoke(static_cast<Tag&&>(tag), static_cast<Args&&>(args)...)))
      -> decltype(tag_invoke(
          static_cast<Tag&&>(tag), static_cast<Args&&>(args)...)) {
    return tag_invoke(static_cast<Tag&&>(tag), static_cast<Args&&>(args)...);
  }
};

// Manually implement the traits here rather than defining them in terms of
// the corresponding std::invoke_result/is_invocable/is_nothrow_invocable
// traits to improve compile-times. We don't need all of the generality of
// the std:: traits and the tag_invoke traits can be used heavily in CPO-based
// code so optimising them for compile times can make a big difference.

// Use the immediately-invoked function-pointer trick here to avoid
// instantiating the std::declval<T>() template.

template <typename Tag, typename... Args>
using tag_invoke_result_t = decltype(tag_invoke(
    static_cast<Tag && (*)() noexcept>(nullptr)(),
    static_cast<Args && (*)() noexcept>(nullptr)()...));

template <typename Tag, typename... Args>
auto try_tag_invoke(int) noexcept(
    noexcept(tag_invoke(FOLLY_DECLVAL(Tag&&), FOLLY_DECLVAL(Args&&)...)))
    -> decltype(static_cast<void>(tag_invoke(FOLLY_DECLVAL(Tag&&), FOLLY_DECLVAL(Args&&)...)), std::true_type{});

template <typename Tag, typename... Args>
std::false_type try_tag_invoke(...) noexcept(false);

template <template <typename...> class T, typename... Args>
struct defer {
  using type = T<Args...>;
};

struct empty {};

} // namespace detail_tag_invoke_fn

// The expression folly::tag_invoke(tag, args...) is equivalent to performing
// a call to the expression tag_invoke(tag, args...) using argument-dependent
// lookup (ADL).
//
// This is intended to be used by customization-point objects, which dispatch
// a call to the CPO to an ADL call to tag_invoke(cpo, args...), using the type
// of the first argument to disambiguate between customisations for different
// CPOs rather than using different ADL names for this.
//
// For example: Defining a new CPO in terms of tag_invoke.
//   struct FooCpo {
//     template<typename A, typename B>
//     auto operator()(A&& a, B&& b) const
//         noexcept(folly::is_nothrow_tag_invocable_v<FooCpo, A, B>)
//         -> folly::tag_invoke_result_t<FooCpo, A, B> {
//       return folly::tag_invoke(*this, (A&&)a, (B&&)b);
//     }
//   };
//   FOLLY_DEFINE_CPO(FooCpo, Foo)
//
// And then customising the Foo CPO for a particular type:
//   class SomeType {
//     ...
//     template<typename B>
//     friend int tag_invoke(folly::cpo_t<Foo>, const SomeType& a, B&& b) {
//       // implementation goes here
//     }
//   };
//
// For more details see the C++ standards proposal: https://wg21.link/P1895R0.
FOLLY_DEFINE_CPO(detail_tag_invoke_fn::tag_invoke_fn, tag_invoke)

// Query if the 'folly::tag_invoke()' CPO can be invoked with a tag and
// arguments of the the specified types.
//
// This checks whether an overload of the free-function tag_invoke() found
// by ADL can be invoked with the specified types.

template <typename Tag, typename... Args>
inline constexpr bool is_tag_invocable_v =
    decltype(detail_tag_invoke_fn::try_tag_invoke<Tag, Args...>(0))::value;

template <typename Tag, typename... Args>
struct is_tag_invocable //
    : std::bool_constant<is_tag_invocable_v<Tag, Args...>> {};

// Query whether the 'folly::tag_invoke()' CPO can be invoked with a tag
// and arguments of the specified type and that such an invocation is
// noexcept.

template <typename Tag, typename... Args>
inline constexpr bool is_nothrow_tag_invocable_v =
    noexcept(detail_tag_invoke_fn::try_tag_invoke<Tag, Args...>(0));

template <typename Tag, typename... Args>
struct is_nothrow_tag_invocable
    : std::bool_constant<is_nothrow_tag_invocable_v<Tag, Args...>> {};

// Versions of the above that check in addition that the result is
// convertible to the given return type R.

template <typename R, typename Tag, typename... Args>
using is_tag_invocable_r =
    folly::is_invocable_r<R, decltype(folly::tag_invoke), Tag, Args...>;

template <typename R, typename Tag, typename... Args>
inline constexpr bool is_tag_invocable_r_v =
    is_tag_invocable_r<R, Tag, Args...>::value;

template <typename R, typename Tag, typename... Args>
using is_nothrow_tag_invocable_r =
    folly::is_nothrow_invocable_r<R, decltype(folly::tag_invoke), Tag, Args...>;

template <typename R, typename Tag, typename... Args>
inline constexpr bool is_nothrow_tag_invocable_r_v =
    is_nothrow_tag_invocable_r<R, Tag, Args...>::value;

using detail_tag_invoke_fn::tag_invoke_result_t;

template <typename Tag, typename... Args>
struct tag_invoke_result
    : conditional_t<
          is_tag_invocable_v<Tag, Args...>,
          detail_tag_invoke_fn::defer<tag_invoke_result_t, Tag, Args...>,
          detail_tag_invoke_fn::empty> {};

/// passable_to
///
/// Useful for enabling n^k overloads all at once. Example below shows cases of
/// n=5 with k=1 and k=2.
///
/// Useful for transparent hash and key-equal functions for a set which needs
/// heterogeneous lookup.
///
/// Law:
///   passable_to<Arg, Fun> = invocable<Fun, Arg>
///
/// Discussion:
/// Q:  Why require a one-param function type and not support a multi-param
///     function type?
/// A:  Too much arbitrary choice. Interface would be confusing.
///     To provide an interface like:
///         passable_to<Slog, Fun, Arg...>
///     Then `Arg...` would need exactly one "hole", which would be represented
///     by perhaps `void` or perhaps `std::placeholder::_1` or another sentinel.
///     Alternatively:
///         passable_to<Slog, Fun, tag_t<ArgFront...>, tag_t<ArgBack...>>
///     Now the usage gets convoluted.
///     But, in the end, this can be worked around by passing a function type
///     that is the result of std::bind_front (C++20) or std::bind_back (C++23).
///
/// Example:
///
///     struct to_key_fn {
///       string_view operator()(string_view val) const noexcept { return val; }
///       string_view operator()(object const&) const noexcept;
///       string_view operator()(object const*) const noexcept;
///       string_view operator()(unique_ptr<object> const&) const noexcept;
///       string_view operator()(shared_ptr<object> const&) const noexcept;
///     };
///     static constexpr to_key_fn to_key{};
///
///     struct obj_hash : hash<string_view> {
///       using is_transparent = void;
///       size_t operator()(passable_to<to_key_fn> auto const& val) noexcept {
///         return hash<string_view>::operator()(to_key(val));
///       }
///     };
///     struct obj_key_equal : equal_to<string_view> {
///       using is_transparent = void;
///       bool operator()(
///           passable_to<to_key_fn> auto const& lhs,
///           passable_to<to_key_fn> auto const& rhs) noexcept {
///         return equal_to<string_view>::operator()(to_key(lhs), to_key(rhs));
///       }
///     };
///
///     using object_set = std::unordered_set<
///         std::shared_ptr<object>,
///         obj_hash,
///         obj_key_equal>;
///     object_set objs;
template <typename Arg, typename Fun>
concept passable_to = invoke_detail::is_invocable_v<Fun, Arg>;

} // namespace folly
