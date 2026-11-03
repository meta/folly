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

#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

#include <folly/CPortability.h>
#include <folly/Portability.h>
#include <folly/Traits.h>

namespace folly {

/*
 * FOLLY_DECLVAL(T)
 *
 * This macro works like std::declval<T>() but does the same thing in a way
 * that does not require instantiating a function template.
 *
 * Use this macro instead of std::declval<T>() in places that are widely
 * instantiated to reduce compile-time overhead of instantiating function
 * templates.
 *
 * Note that, like std::declval<T>(), this macro can only be used in
 * unevaluated contexts.
 *
 * There are some small differences between this macro and std::declval<T>().
 * - This macro results in a value of type 'T' instead of 'T&&'.
 * - This macro requires the type T to be a complete type at the
 *   point of use.
 *   If this is a problem then use FOLLY_DECLVAL(T&&) instead, or if T might
 *   be 'void', then use FOLLY_DECLVAL(std::add_rvalue_reference_t<T>).
 */
#define FOLLY_DECLVAL(...) static_cast<__VA_ARGS__ (*)() noexcept>(nullptr)()

namespace detail {
template <typename T>
T decay_1_(T const volatile&&);
template <typename T>
T decay_1_(T const&);
template <typename T>
T* decay_1_(T*);

template <typename T>
auto decay_0_(int) -> decltype(detail::decay_1_(FOLLY_DECLVAL(T&&)));
template <typename T>
auto decay_0_(short) -> void;

template <typename T>
using decay_t = decltype(detail::decay_0_<T>(0));
} // namespace detail

//  decay_t
//
//  Like std::decay_t but possibly faster to compile.
//
//  mimic: std::decay_t, C++14
using detail::decay_t;

/**
 *  copy
 *
 *  Usable when you have a function with two overloads:
 *
 *      class MyData;
 *      void something(MyData&&);
 *      void something(const MyData&);
 *
 *  Where the purpose is to make copies and moves explicit without having to
 *  spell out the full type names - in this case, for copies, to invoke copy
 *  constructors.
 *
 *  When the caller wants to pass a copy of an lvalue, the caller may:
 *
 *      void foo() {
 *        MyData data;
 *        something(folly::copy(data)); // explicit copy
 *        something(std::move(data)); // explicit move
 *        something(data); // const& - neither move nor copy
 *      }
 *
 *  Note: If passed an rvalue, invokes the move-ctor, not the copy-ctor. This
 *  can be used to to force a move, where just using std::move would not:
 *
 *      folly::copy(std::move(data)); // force-move, not just a cast to &&
 *
 *  Note: The following text appears in the standard:
 *
 *      In several places in this Clause the operation //DECAY_COPY(x)// is
 *      used. All such uses mean call the function `decay_copy(x)` and use the
 *      result, where `decay_copy` is defined as follows:
 *
 *        template <class T> decay_t<T> decay_copy(T&& v)
 *          { return std::forward<T>(v); }
 *
 *      http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n4296.pdf
 *        30.2.6 `decay_copy` [thread.decaycopy].
 *
 *  We mimic it, with a `noexcept` specifier for good measure.
 */

template <typename T>
constexpr detail::decay_t<T> copy(T&& value) noexcept(
    noexcept(detail::decay_t<T>(static_cast<T&&>(value)))) {
  return static_cast<T&&>(value);
}

//  mimic: `std::forward_like`, C++23 / p2445r0.
template <typename Src, typename Dst>
constexpr like_t<Src, Dst>&& forward_like(Dst&& dst) noexcept {
  return std::forward<like_t<Src, Dst>>(static_cast<Dst&&>(dst));
}

/**
 * Initializer lists are a powerful compile time syntax introduced in C++11
 * but due to their often conflicting syntax they are not used by APIs for
 * construction.
 *
 * Further standard conforming compilers *strongly* favor an
 * std::initializer_list overload for construction if one exists.  The
 * following is a simple tag used to disambiguate construction with
 * initializer lists and regular uniform initialization.
 *
 * For example consider the following case
 *
 *  class Something {
 *  public:
 *    explicit Something(int);
 *    Something(std::initializer_list<int>);
 *
 *    operator int();
 *  };
 *
 *  ...
 *  Something something{1}; // SURPRISE!!
 *
 * The last call to instantiate the Something object will go to the
 * initializer_list overload.  Which may be surprising to users.
 *
 * If however this tag was used to disambiguate such construction it would be
 * easy for users to see which construction overload their code was referring
 * to.  For example
 *
 *  class Something {
 *  public:
 *    explicit Something(int);
 *    Something(folly::initlist_construct_t, std::initializer_list<int>);
 *
 *    operator int();
 *  };
 *
 *  ...
 *  Something something_one{1}; // not the initializer_list overload
 *  Something something_two{folly::initlist_construct, {1}}; // correct
 */
struct initlist_construct_t {};
constexpr initlist_construct_t initlist_construct{};

//  sorted_unique_t, sorted_unique
//
//  A generic tag type and value to indicate that some constructor or method
//  accepts a container in which the values are sorted and unique.
//
//  Example:
//
//    void takes_numbers(folly::sorted_unique_t, std::vector<int> alist) {
//      assert(std::is_sorted(alist.begin(), alist.end()));
//      assert(std::unique(alist.begin(), alist.end()) == alist.end());
//      for (i : alist) {
//        // some behavior which safe only when alist is sorted and unique
//      }
//    }
//    void takes_numbers(std::vector<int> alist) {
//      std::sort(alist.begin(), alist.end());
//      alist.erase(std::unique(alist.begin(), alist.end()), alist.end());
//      takes_numbers(folly::sorted_unique, alist);
//    }
//
//  mimic: std::sorted_unique_t, std::sorted_unique, p0429r6
struct sorted_unique_t {};
constexpr sorted_unique_t sorted_unique{};

//  sorted_equivalent_t, sorted_equivalent
//
//  A generic tag type and value to indicate that some constructor or method
//  accepts a container in which the values are sorted but not necessarily
//  unique.
//
//  Example:
//
//    void takes_numbers(folly::sorted_equivalent_t, std::vector<int> alist) {
//      assert(std::is_sorted(alist.begin(), alist.end()));
//      for (i : alist) {
//        // some behavior which safe only when alist is sorted
//      }
//    }
//    void takes_numbers(std::vector<int> alist) {
//      std::sort(alist.begin(), alist.end());
//      takes_numbers(folly::sorted_equivalent, alist);
//    }
//
//  mimic: std::sorted_equivalent_t, std::sorted_equivalent, p0429r6
struct sorted_equivalent_t {};
constexpr sorted_equivalent_t sorted_equivalent{};

/// nontype_t
/// nontype
///
/// A tag type and value carrying a value as a non-type template argument, for
/// passing a callable known at compile time to a function wrapper which then
/// need not reference it.
///
/// Example:
///
///   folly::FunctionRef<int()> f = folly::nontype<[] { return 42; }>;
///
/// mimic: std::nontype_t, std::nontype, p0792r14
template <auto V>
struct nontype_t {
  explicit nontype_t() = default;
};
template <auto V>
inline constexpr nontype_t<V> nontype{};

template <typename T>
struct transparent : T {
  using is_transparent = void;
  using T::T;
};

/**
 * A simple function object that passes its argument through unchanged.
 *
 * Example:
 *
 *   int i = 42;
 *   int &j = identity(i);
 *   assert(&i == &j);
 *
 * Warning: passing a prvalue through identity turns it into an xvalue,
 * which can effect whether lifetime extension occurs or not. For instance:
 *
 *   auto&& x = std::make_unique<int>(42);
 *   cout << *x ; // OK, x refers to a valid unique_ptr.
 *
 *   auto&& y = identity(std::make_unique<int>(42));
 *   cout << *y ; // ERROR: y did not lifetime-extend the unique_ptr. It
 *                // is no longer valid
 */
struct identity_fn {
  template <class T>
  constexpr T&& operator()(T&& x) const noexcept {
    return static_cast<T&&>(x);
  }
};
using Identity = identity_fn;
inline constexpr identity_fn identity{};

#if !defined(__NVCC__)

/// literal_c_str
///
/// This can only wrap literal strings, since the constructor is marked
/// consteval.  Like with fmt::format_string.
struct literal_c_str {
  const char* const ptr;
  /* implicit */ consteval literal_c_str(const char* p) : ptr(p) {}
};

#endif

#if !defined(__NVCC__)

/// literal_string
///
/// A structural type representing a literal string. A structural type may be
/// a non-type template argument.
///
/// May at times be useful since language-level literal strings are not allowed
/// as non-type template arguments.
///
/// This may typically be used with vtag for passing the literal string as a
/// constant-expression via a non-type template argument.
///
/// Example:
///
///   template <size_t N, literal_string<char, N> Str>
///   void do_something_with_literal_string(vtag_t<Str>);
///
///   void do_something() {
///     do_something_with_literal_string(vtag<literal_string{"foobar"}>);
///   }
template <typename C, std::size_t N>
struct literal_string {
  C buffer[N] = {};

  consteval /* implicit */ literal_string(C const (&buf)[N]) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
      buffer[i] = buf[i];
    }
  }

  constexpr std::size_t size() const noexcept { return N - 1; }
  constexpr C const* data() const noexcept { return buffer; }
  constexpr C const* c_str() const noexcept { return buffer; }

  template <
      typename String,
      decltype((void(String(FOLLY_DECLVAL(C const*), N - 1)), 0)) = 0>
  constexpr explicit operator String() const //
      noexcept(noexcept(String(FOLLY_DECLVAL(C const*), N - 1))) {
    return String(data(), N - 1);
  }
};

inline namespace literals {
inline namespace string_literals {

template <literal_string Str>
consteval decltype(Str) operator""_lit() noexcept {
  return Str;
}
template <literal_string Str>
consteval vtag_t<Str> operator""_litv() noexcept {
  return vtag<Str>;
}

} // namespace string_literals
} // namespace literals

#endif

namespace detail {

template <typename T>
struct inheritable_inherit_ : T {
  using T::T;
  template <
      typename... A,
      std::enable_if_t<std::is_constructible<T, A...>::value, int> = 0>
  /* implicit */ FOLLY_ERASE inheritable_inherit_(A&&... a) noexcept(
      noexcept(T(static_cast<A&&>(a)...)))
      : T(static_cast<A&&>(a)...) {}
};

template <typename T>
struct inheritable_contain_ {
  T v;
  template <
      typename... A,
      std::enable_if_t<std::is_constructible<T, A...>::value, int> = 0>
  /* implicit */ FOLLY_ERASE inheritable_contain_(A&&... a) noexcept(
      noexcept(T(static_cast<A&&>(a)...)))
      : v(static_cast<A&&>(a)...) {}
  FOLLY_ERASE operator T&() & noexcept { return v; }
  FOLLY_ERASE operator T&&() && noexcept { return static_cast<T&&>(v); }
  FOLLY_ERASE operator T const&() const& noexcept { return v; }
  FOLLY_ERASE operator T const&&() const&& noexcept {
    return static_cast<T const&&>(v);
  }
};

template <typename T>
constexpr bool is_inheritable_v_ = //
    (std::is_class_v<T> || std::is_union_v<T>) &&
    !(std::is_abstract_v<T> || std::is_final_v<T>);

template <bool>
struct inheritable_;
template <>
struct inheritable_<false> {
  template <typename T>
  using apply = inheritable_contain_<T>;
};
template <>
struct inheritable_<true> {
  template <typename T>
  using apply = inheritable_inherit_<T>;
};

//  inheritable
//
//  A class wrapping an arbitrary type T which is always inheritable, and which
//  enables empty-base-optimization when possible.
template <typename T, bool C = is_inheritable_v_<T>>
using inheritable = typename inheritable_<C>::template apply<T>;

} // namespace detail

// Prevent child classes from finding anything in folly:: by ADL.
namespace moveonly_ {

/**
 * Disallow copy but not move in derived types. This is essentially
 * boost::noncopyable (the implementation is almost identical), except:
 * 1) It doesn't delete move constructor and move assignment.
 * 2) It has public methods, enabling aggregate initialization.
 */
struct MoveOnly {
  constexpr MoveOnly() noexcept = default;
  ~MoveOnly() noexcept = default;

  MoveOnly(MoveOnly&&) noexcept = default;
  MoveOnly& operator=(MoveOnly&&) noexcept = default;
  MoveOnly(const MoveOnly&) = delete;
  MoveOnly& operator=(const MoveOnly&) = delete;
};

/**
 * Disallow copy and move for derived types. This is essentially
 * boost::noncopyable (the implementation is almost identical), except it has
 * public methods, enabling aggregate initialization.
 */
struct NonCopyableNonMovable {
  constexpr NonCopyableNonMovable() noexcept = default;
  ~NonCopyableNonMovable() noexcept = default;

  NonCopyableNonMovable(NonCopyableNonMovable&&) = delete;
  NonCopyableNonMovable& operator=(NonCopyableNonMovable&&) = delete;
  NonCopyableNonMovable(const NonCopyableNonMovable&) = delete;
  NonCopyableNonMovable& operator=(const NonCopyableNonMovable&) = delete;
};

struct Default {};

template <bool Copy, bool Move>
using EnableCopyMove = std::conditional_t<
    Copy,
    Default,
    std::conditional_t<Move, MoveOnly, NonCopyableNonMovable>>;

} // namespace moveonly_

using moveonly_::MoveOnly;
using moveonly_::NonCopyableNonMovable;

/// variadic_noop
/// variadic_noop_fn
///
/// An invocable object and type that has no side-effects - that does nothing
/// when invoked regardless of the arguments with which it is invoked - and that
/// returns void.
///
/// May be invoked with any arguments. Returns void.
struct variadic_noop_fn {
  template <typename... A>
  constexpr void operator()(A&&... /*unused*/) const noexcept {}
};
inline constexpr variadic_noop_fn variadic_noop;

/// variadic_constant_of
/// variadic_constant_of_fn
///
/// An invocable object and type that has no side-effects - that does nothing
/// when invoked regardless of the arguments with which it is invoked - and that
/// returns a constant value.
template <auto Value>
struct variadic_constant_of_fn {
  using value_type = decltype(Value);
  static inline constexpr value_type value = Value;
  template <typename... A>
  constexpr value_type operator()(A&&... /*unused*/) const noexcept {
    return value;
  }
};
template <auto Value>
inline constexpr variadic_constant_of_fn<Value> variadic_constant_of;

//  unsafe_default_initialized
//  unsafe_default_initialized_cv
//
//  An object which is explicitly convertible to any default-constructible type
//  and which, upon conversion, yields a default-initialized value of that type.
//
//  https://en.cppreference.com/w/cpp/language/default_initialization
//
//  For fundamental types, a default-initialized instance may have indeterminate
//  value. Reading an indeterminate value is undefined behavior but may offer a
//  performance optimization. When using an indeterminate value as a performance
//  optimization, it is best to be explicit.
//
//  Useful as an escape hatch when enabling warnings or errors:
//  * gcc:
//    * uninitialized
//    * maybe-uninitialized
//  * clang:
//    * uninitialized
//    * conditional-uninitialized
//    * sometimes-uninitialized
//    * uninitialized-const-reference
//  * msvc:
//    * C4701: potentially uninitialized local variable used
//    * C4703: potentially uninitialized local pointer variable used
//
//  Example:
//
//      int local = folly::unsafe_default_initialized;
//      store_value_into_int_ptr(&value); // suppresses possible warning
//      use_value(value); // suppresses possible warning
struct unsafe_default_initialized_cv {
  FOLLY_PUSH_WARNING
  // MSVC requires warning disables to be outside of function definition
  // Uninitialized local variable 'uninit' used
  FOLLY_MSVC_DISABLE_WARNING(4700)
  // Potentially uninitialized local variable 'uninit' used
  FOLLY_MSVC_DISABLE_WARNING(4701)
  // Potentially uninitialized local pointer variable 'uninit' used
  FOLLY_MSVC_DISABLE_WARNING(4703)
  // Using uninitialized memory `uninit`
  FOLLY_MSVC_DISABLE_WARNING(6001)
  FOLLY_GNU_DISABLE_WARNING("-Wuninitialized")
  // Clang doesn't implement -Wmaybe-uninitialized and warns about it
  FOLLY_GCC_DISABLE_WARNING("-Wmaybe-uninitialized")
  template <typename T>
  FOLLY_ERASE constexpr /* implicit */ operator T() const noexcept {
#if (defined(_MSC_VER) && !defined(__MSVC_RUNTIME_CHECKS)) || \
    (defined(__clang__) && !defined(__GNUC__))
    if (!std::is_constant_evaluated()) {
      T uninit;
      return uninit;
    }
#endif
    return T();
  }
  FOLLY_POP_WARNING
};
inline constexpr unsafe_default_initialized_cv unsafe_default_initialized{};

/// to_bool
/// to_bool_fn
///
/// Constructs a boolean from the argument.
///
/// Particularly useful for testing sometimes-weak function declarations. They
/// may be declared weak on some platforms but not on others. GCC likes to warn
/// about them but the warning is unhelpful.
struct to_bool_fn {
  template <typename..., typename T>
  FOLLY_ERASE constexpr auto operator()(T const& t) const noexcept
      -> decltype(static_cast<bool>(t)) {
    FOLLY_PUSH_WARNING
    FOLLY_GCC_DISABLE_WARNING("-Waddress")
    FOLLY_GCC_DISABLE_WARNING("-Wnonnull-compare")
    return static_cast<bool>(t);
    FOLLY_POP_WARNING
  }
};
inline constexpr to_bool_fn to_bool{};

struct to_signed_fn {
  template <typename..., typename T>
  constexpr auto operator()(T const& t) const noexcept ->
      typename std::make_signed<T>::type {
    using S = typename std::make_signed<T>::type;
    // note: static_cast<S>(t) would be more straightforward, but it would also
    // be implementation-defined behavior and that is typically to be avoided;
    // the following code optimized into the same thing, though
    constexpr auto m = static_cast<T>(std::numeric_limits<S>::max());
    return static_cast<S>(m < t ? -static_cast<S>(~t) + S{-1} : t);
  }
};
inline constexpr to_signed_fn to_signed{};

struct to_unsigned_fn {
  template <typename..., typename T>
  constexpr auto operator()(T const& t) const noexcept ->
      typename std::make_unsigned<T>::type {
    using U = typename std::make_unsigned<T>::type;
    return static_cast<U>(t);
  }
};
inline constexpr to_unsigned_fn to_unsigned{};

namespace detail {
template <typename Src, typename Dst>
inline constexpr bool is_to_narrow_convertible_v =
    (std::is_integral<Dst>::value) &&
    (std::is_signed<Dst>::value == std::is_signed<Src>::value);
}

template <typename Src>
class to_narrow_convertible {
  static_assert(std::is_integral<Src>::value, "not an integer");

  template <typename Dst>
  struct to_
      : std::bool_constant<detail::is_to_narrow_convertible_v<Src, Dst>> {};

 public:
  explicit constexpr to_narrow_convertible(Src const& value) noexcept
      : value_(value) {}
  explicit to_narrow_convertible(to_narrow_convertible const&) = default;
  explicit to_narrow_convertible(to_narrow_convertible&&) = default;
  to_narrow_convertible& operator=(to_narrow_convertible const&) = default;
  to_narrow_convertible& operator=(to_narrow_convertible&&) = default;

  template <typename Dst, std::enable_if_t<to_<Dst>::value, int> = 0>
  /* implicit */ constexpr operator Dst() const noexcept {
    FOLLY_PUSH_WARNING
    FOLLY_MSVC_DISABLE_WARNING(4244) // lossy conversion: arguments
    FOLLY_MSVC_DISABLE_WARNING(4267) // lossy conversion: variables
    FOLLY_GNU_DISABLE_WARNING("-Wconversion")
    return value_;
    FOLLY_POP_WARNING
  }

 private:
  Src value_;
};

//  to_narrow
//
//  A utility for performing explicit possibly-narrowing integral conversion
//  without specifying the destination type. Does not permit changing signs.
//  Sometimes preferable to static_cast<Dst>(src) to document the intended
//  semantics of the cast.
//
//  Models explicit conversion with an elided destination type. Sits in between
//  a stricter explicit conversion with a named destination type and a more
//  lenient implicit conversion. Implemented with implicit conversion in order
//  to take advantage of the undefined-behavior sanitizer's inspection of all
//  implicit conversions - it checks for truncation, with suppressions in place
//  for warnings which guard against narrowing implicit conversions.
struct to_narrow_fn {
  template <typename..., typename Src>
  constexpr auto operator()(Src const& src) const noexcept
      -> to_narrow_convertible<Src> {
    return to_narrow_convertible<Src>{src};
  }
};
inline constexpr to_narrow_fn to_narrow{};

template <typename Src>
class to_integral_convertible {
  static_assert(std::is_floating_point<Src>::value, "not a floating-point");

  template <typename Dst>
  static constexpr bool to_ = std::is_integral<Dst>::value;

 public:
  explicit constexpr to_integral_convertible(Src const& value) noexcept
      : value_(value) {}

  explicit to_integral_convertible(to_integral_convertible const&) = default;
  explicit to_integral_convertible(to_integral_convertible&&) = default;
  to_integral_convertible& operator=(to_integral_convertible const&) = default;
  to_integral_convertible& operator=(to_integral_convertible&&) = default;

  template <typename Dst, std::enable_if_t<to_<Dst>, int> = 0>
  /* implicit */ constexpr operator Dst() const noexcept {
    FOLLY_PUSH_WARNING
    FOLLY_MSVC_DISABLE_WARNING(4244) // lossy conversion: arguments
    FOLLY_MSVC_DISABLE_WARNING(4267) // lossy conversion: variables
    FOLLY_GNU_DISABLE_WARNING("-Wconversion")
    return value_;
    FOLLY_POP_WARNING
  }

 private:
  Src value_;
};

//  to_integral
//
//  A utility for performing explicit floating-point-to-integral conversion
//  without specifying the destination type. Sometimes preferable to
//  static_cast<Dst>(src) to document the intended semantics of the cast.
//
//  Models explicit conversion with an elided destination type. Sits in between
//  a stricter explicit conversion with a named destination type and a more
//  lenient implicit conversion. Implemented with implicit conversion in order
//  to take advantage of the undefined-behavior sanitizer's inspection of all
//  implicit conversions.
struct to_integral_fn {
  template <typename..., typename Src>
  constexpr auto operator()(Src const& src) const noexcept
      -> to_integral_convertible<Src> {
    return to_integral_convertible<Src>{src};
  }
};
inline constexpr to_integral_fn to_integral{};

template <typename Src>
class to_floating_point_convertible {
  static_assert(std::is_integral<Src>::value, "not a floating-point");

  template <typename Dst>
  static constexpr bool to_ = std::is_floating_point<Dst>::value;

 public:
  explicit constexpr to_floating_point_convertible(Src const& value) noexcept
      : value_(value) {}

  explicit to_floating_point_convertible(to_floating_point_convertible const&) =
      default;
  explicit to_floating_point_convertible(to_floating_point_convertible&&) =
      default;
  to_floating_point_convertible& operator=(
      to_floating_point_convertible const&) = default;
  to_floating_point_convertible& operator=(to_floating_point_convertible&&) =
      default;

  template <typename Dst, std::enable_if_t<to_<Dst>, int> = 0>
  /* implicit */ constexpr operator Dst() const noexcept {
    FOLLY_PUSH_WARNING
    FOLLY_GNU_DISABLE_WARNING("-Wconversion")
    return value_;
    FOLLY_POP_WARNING
  }

 private:
  Src value_;
};

//  to_floating_point
//
//  A utility for performing explicit integral-to-floating-point conversion
//  without specifying the destination type. Sometimes preferable to
//  static_cast<Dst>(src) to document the intended semantics of the cast.
//
//  Models explicit conversion with an elided destination type. Sits in between
//  a stricter explicit conversion with a named destination type and a more
//  lenient implicit conversion. Implemented with implicit conversion in order
//  to take advantage of the undefined-behavior sanitizer's inspection of all
//  implicit conversions.
struct to_floating_point_fn {
  template <typename..., typename Src>
  constexpr auto operator()(Src const& src) const noexcept
      -> to_floating_point_convertible<Src> {
    return to_floating_point_convertible<Src>{src};
  }
};
inline constexpr to_floating_point_fn to_floating_point{};

struct to_underlying_fn {
  template <typename..., class E>
  constexpr std::underlying_type_t<E> operator()(E e) const noexcept {
    static_assert(std::is_enum<E>::value, "not an enum type");
    return static_cast<std::underlying_type_t<E>>(e);
  }
};
inline constexpr to_underlying_fn to_underlying{};

namespace detail {
template <typename R>
using invocable_to_detect = decltype(FOLLY_DECLVAL(R)());

template <
    typename F,
    //  MSVC 14.16.27023 does not permit these to be in the class body:
    //    error C2833: 'operator decltype' is not a recognized operator or type
    //  TODO: return these to the class body and remove the static assertions
    typename TML = detected_t<invocable_to_detect, F&>,
    typename TCL = detected_t<invocable_to_detect, F const&>,
    typename TMR = detected_t<invocable_to_detect, F&&>,
    typename TCR = detected_t<invocable_to_detect, F const&&>>
class invocable_to_convertible : private inheritable<F> {
 private:
  static_assert(std::is_same<F, decay_t<F>>::value, "mismatch");

  using base = inheritable<F>;

  template <typename R>
  using result_t = detected_t<invocable_to_detect, R>;
  template <typename R>
  static constexpr bool detected_v = is_detected_v<invocable_to_detect, R>;
  template <typename R>
  using if_invocable_as_v = std::enable_if_t<detected_v<R>, int>;
  template <typename R>
  static constexpr bool nx_v = noexcept(FOLLY_DECLVAL(R)());
  template <typename G>
  static constexpr bool constructible_v = std::is_constructible<F, G&&>::value;

  using FML = F&;
  using FCL = F const&;
  using FMR = F&&;
  using FCR = F const&&;
  static_assert(std::is_same<TML, result_t<FML>>::value, "mismatch");
  static_assert(std::is_same<TCL, result_t<FCL>>::value, "mismatch");
  static_assert(std::is_same<TMR, result_t<FMR>>::value, "mismatch");
  static_assert(std::is_same<TCR, result_t<FCR>>::value, "mismatch");

  using BML = base&;
  using BCL = base const&;
  using BMR = base&&;
  using BCR = base const&&;

 public:
  template <typename G, std::enable_if_t<constructible_v<G&&>, int> = 0>
  FOLLY_ERASE explicit constexpr invocable_to_convertible(G&& g) noexcept(
      noexcept(F(static_cast<G&&>(g))))
      : inheritable<F>(static_cast<G&&>(g)) {}

  template <typename..., typename R = FML, if_invocable_as_v<R> = 0>
  FOLLY_ERASE constexpr operator TML() & noexcept(nx_v<R>) {
    return static_cast<FML>(static_cast<BML>(*this))();
  }
  template <typename..., typename R = FCL, if_invocable_as_v<R> = 0>
  FOLLY_ERASE constexpr operator TCL() const& noexcept(nx_v<R>) {
    return static_cast<FCL>(static_cast<BCL>(*this))();
  }
  template <typename..., typename R = FMR, if_invocable_as_v<R> = 0>
  FOLLY_ERASE constexpr operator TMR() && noexcept(nx_v<R>) {
    return static_cast<FMR>(static_cast<BMR>(*this))();
  }
  template <typename..., typename R = FCR, if_invocable_as_v<R> = 0>
  FOLLY_ERASE constexpr operator TCR() const&& noexcept(nx_v<R>) {
    return static_cast<FCR>(static_cast<BCR>(*this))();
  }
};
} // namespace detail

//  invocable_to
//  invocable_to_fn
//
//  Given an invocable, returns an object which is implicitly convertible to the
//  type which the invocable returns when invoked with no arguments. Conversion
//  invokes the invocables and returns the value.
//
//  The return object has unspecified type with the following semantics:
//  * It stores a decay-copy of the passed invocable.
//  * It defines four-way conversion operators. Each conversion operator purely
//    forwards to the invocable as forwarded-like the convertible, and has the
//    same exception specification and the same participation in overload
//    resolution as invocation of the invocable.
//
//  Example:
//
//    Given a setup:
//
//      struct stable {
//        int value = 0;
//        stable() = default;
//        stable(stable const&); // expensive!
//      };
//      std::list<stable const> list;
//
//    The goal is to insert a stable with a value of 7 to the back of the list.
//
//    The obvious ways are expensive:
//
//      stable obj;
//      obj.value = 7;
//      list.push_back(obj); // or variations with emplace_back or std::move
//
//    With a lambda and copy elision optimization (NRVO), the expense remains:
//
//      list.push_back(std::invoke([] {
//        stable obj;
//        obj.value = 7;
//        return obj;
//      }));
//
//    But conversion, as done with this utility, makes this goal achievable.
//
//      list.emplace_back(folly::invocable_to([] {
//        stable obj;
//        obj.value = 7;
//        return obj;
//      }));
struct invocable_to_fn {
  template <
      typename F,
      typename...,
      typename D = detail::decay_t<F>,
      typename R = detail::invocable_to_convertible<D>,
      std::enable_if_t<std::is_constructible<D, F&&>::value, int> = 0>
  FOLLY_ERASE constexpr R operator()(F&& f) const
      noexcept(noexcept(R(static_cast<F&&>(f)))) {
    return R(static_cast<F&&>(f));
  }
};
inline constexpr invocable_to_fn invocable_to{};

/// object_from_member
/// object_from_member_fn
///
/// Returns the object containing the given field.
///
/// Similar to container_of (linux kernel).
///
/// Example:
///
///   using obj_t = std::pair<int, float>;
///   obj_t obj = {1, 3.0};
///   assert(&obj == object_from_member(obj_t::second, &obj.second);
struct object_from_member_fn {
 private:
  template <typename M, typename O>
  using ptr_t = M O::*;

  template <typename M, typename O>
  static std::ptrdiff_t off(ptr_t<M, O> const p) noexcept {
    O* o = nullptr;
    M* m = &(o->*p);
    return reinterpret_cast<char*>(m) - reinterpret_cast<char*>(o);
  }

 public:
  template <typename M, typename O>
  O* operator()(ptr_t<M, O> const p, M* m) const noexcept {
    return reinterpret_cast<O*>(reinterpret_cast<char*>(m) - off(p));
  }
  template <typename M, typename O>
  O const* operator()(ptr_t<M, O> const p, M const* m) const noexcept {
    return reinterpret_cast<O*>(reinterpret_cast<char const*>(m) - off(p));
  }

  template <typename M, typename O>
  O& operator()(ptr_t<M, O> const p, M& m) const noexcept {
    return *operator()(p, &m);
  }
  template <typename M, typename O>
  O const& operator()(ptr_t<M, O> const p, M const& m) const noexcept {
    return *operator()(p, &m);
  }
  template <typename M, typename O>
  O&& operator()(ptr_t<M, O> const p, M&& m) const noexcept {
    return static_cast<O&&>(*operator()(p, &m));
  }
  template <typename M, typename O>
  O const&& operator()(ptr_t<M, O> const p, M const& m) const noexcept {
    return static_cast<O const&&>(*operator()(p, &m));
  }
};
inline constexpr object_from_member_fn object_from_member{};

#define FOLLY_DETAIL_FORWARD_BODY(...)                     \
  noexcept(noexcept(__VA_ARGS__))->decltype(__VA_ARGS__) { \
    return __VA_ARGS__;                                    \
  }

/// FOLLY_FOR_EACH_THIS_OVERLOAD_IN_CLASS_BODY_DELEGATE
///
/// Helper macro to add 4 delegated, qualifier-overloaded methods to a class
///
/// Example:
///
///     template <typename T>
///     class optional {
///      public:
///       bool has_value() const;
///
///       T& value() & {
///         if (!has_value()) { throw std::bad_optional_access(); }
///         return m_value;
///       }
///
///       const T& value() const& {
///         if (!has_value()) { throw std::bad_optional_access(); }
///         return m_value;
///       }
///
///       T&& value() && {
///         if (!has_value()) { throw std::bad_optional_access(); }
///         return std::move(m_value);
///       }
///
///       const T&& value() const&& {
///         if (!has_value()) { throw std::bad_optional_access(); }
///         return std::move(m_value);
///       }
///     };
///
/// This is equivalent to
///
///     template <typename T>
///     class optional {
///       template <typename Self>
///       decltype(auto) value_impl(Self&& self) {
///         if (!self.has_value()) {
///           throw std::bad_optional_access();
///         }
///         return std::forward<Self>(self).m_value;
///       }
///       // ...
///
///      public:
///       bool has_value() const;
///
///       FOLLY_FOR_EACH_THIS_OVERLOAD_IN_CLASS_BODY_DELEGATE(value,
///       value_impl);
///     };
///
/// Note: This can be migrated to C++23's deducing this:
/// https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2021/p0847r7.html
///
// clang-format off
#define FOLLY_FOR_EACH_THIS_OVERLOAD_IN_CLASS_BODY_DELEGATE(MEMBER, DELEGATE) \
  template <class... Args>                                                    \
  [[maybe_unused]] FOLLY_ERASE_HACK_GCC                                       \
  constexpr auto MEMBER(Args&&... args) & FOLLY_DETAIL_FORWARD_BODY(          \
      ::std::remove_cvref_t<decltype(*this)>::DELEGATE(                     \
          *this, static_cast<Args&&>(args)...))                               \
  template <class... Args>                                                    \
  [[maybe_unused]] FOLLY_ERASE_HACK_GCC                                       \
  constexpr auto MEMBER(Args&&... args) const& FOLLY_DETAIL_FORWARD_BODY(     \
      ::std::remove_cvref_t<decltype(*this)>::DELEGATE(                     \
          *this, static_cast<Args&&>(args)...))                               \
  template <class... Args>                                                    \
  [[maybe_unused]] FOLLY_ERASE_HACK_GCC                                       \
  constexpr auto MEMBER(Args&&... args) && FOLLY_DETAIL_FORWARD_BODY(         \
      ::std::remove_cvref_t<decltype(*this)>::DELEGATE(                     \
          std::move(*this), static_cast<Args&&>(args)...))                    \
  template <class... Args>                                                    \
  [[maybe_unused]] FOLLY_ERASE_HACK_GCC                                       \
  constexpr auto MEMBER(Args&&... args) const&& FOLLY_DETAIL_FORWARD_BODY(    \
      ::std::remove_cvref_t<decltype(*this)>::DELEGATE(                     \
          std::move(*this), static_cast<Args&&>(args)...))                    \
  /* enforce semicolon after macro */ static_assert(true)
// clang-format on
} // namespace folly
