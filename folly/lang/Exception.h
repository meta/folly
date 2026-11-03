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
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

#if __has_include(<fmt/format.h>)
#include <fmt/format.h>
#endif

#include <folly/CPortability.h>
#include <folly/CppAttributes.h>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/lang/Assume.h>
#include <folly/lang/SafeAssert.h>
#include <folly/lang/Thunk.h>
#include <folly/lang/TypeInfo.h>

namespace folly {

/// throw_exception
///
/// Throw an exception if exceptions are enabled, or terminate if compiled with
/// -fno-exceptions.
template <typename Ex>
[[noreturn, FOLLY_ATTR_GNU_COLD]] FOLLY_NOINLINE void throw_exception(Ex&& ex) {
#if FOLLY_HAS_EXCEPTIONS
  throw static_cast<Ex&&>(ex);
#else
  (void)ex;
  std::terminate();
#endif
}

/// terminate_with
///
/// Terminates as if by forwarding to throw_exception but in a noexcept context.
template <typename Ex>
[[noreturn, FOLLY_ATTR_GNU_COLD]] FOLLY_NOINLINE void terminate_with(
    Ex&& ex) noexcept {
  throw_exception(static_cast<Ex&&>(ex));
}

namespace detail {

struct throw_exception_arg_array_ {
  template <typename R>
  using v = std::remove_extent_t<std::remove_reference_t<R>>;
  template <typename R>
  using apply = std::enable_if_t<std::is_same<char const, v<R>>::value, v<R>*>;
};
struct throw_exception_arg_trivial_ {
  template <typename R>
  using apply = std::remove_cvref_t<R>;
};
struct throw_exception_arg_base_ {
  template <typename R>
  using apply = R;
};
template <typename R>
using throw_exception_arg_ = //
    conditional_t<
        std::is_array<std::remove_reference_t<R>>::value,
        throw_exception_arg_array_,
        conditional_t<
            std::is_trivially_copyable_v<std::remove_cvref_t<R>>,
            throw_exception_arg_trivial_,
            throw_exception_arg_base_>>;
template <typename R>
using throw_exception_arg_t =
    typename throw_exception_arg_<R>::template apply<R>;
template <typename R>
using throw_exception_arg_fmt_t = conditional_t<
    std::is_rvalue_reference_v<throw_exception_arg_t<R>>,
    std::remove_reference_t<throw_exception_arg_t<R>>,
    throw_exception_arg_t<R>>;

template <typename Ex, typename... Args>
[[noreturn, FOLLY_ATTR_GNU_COLD]] FOLLY_NOINLINE void throw_exception_(
    Args... args) {
  throw_exception(Ex(static_cast<Args>(args)...));
}
template <typename Ex, typename... Args>
[[noreturn, FOLLY_ATTR_GNU_COLD]] FOLLY_NOINLINE void terminate_with_(
    Args... args) noexcept {
  throw_exception(Ex(static_cast<Args>(args)...));
}

} // namespace detail

/// throw_exception
///
/// Construct and throw an exception if exceptions are enabled, or terminate if
/// compiled with -fno-exceptions.
///
/// Does not perfectly forward all its arguments. Instead, in the interest of
/// minimizing common-case inline code size, decays its arguments as follows:
/// * refs to arrays of char const are decayed to char const*
/// * refs to arrays are otherwise invalid
/// * refs to trivial types are decayed to values
///
/// The reason for treating refs to arrays as invalid is to avoid having two
/// behaviors for refs to arrays, one for the general case and one for where the
/// inner type is char const. Having two behaviors can be surprising, so avoid.
template <typename Ex, typename... Args>
[[noreturn]] FOLLY_ERASE void throw_exception(Args&&... args) {
  detail::throw_exception_<Ex, detail::throw_exception_arg_t<Args&&>...>(
      static_cast<Args&&>(args)...);
}

/// terminate_with
///
/// Terminates as if by forwarding to throw_exception within a noexcept context.
template <typename Ex, typename... Args>
[[noreturn]] FOLLY_ERASE void terminate_with(Args&&... args) {
  detail::terminate_with_<Ex, detail::throw_exception_arg_t<Args&&>...>(
      static_cast<Args&&>(args)...);
}

#if __has_include(<fmt/format.h>)

namespace detail {

template <typename Ex, typename... Args, typename Str>
[[noreturn, FOLLY_ATTR_GNU_COLD]] FOLLY_NOINLINE void
throw_exception_fmt_format_(Str str, Args... args) {
  auto what = [&] { return fmt::format(str, static_cast<Args&&>(args)...); };
  if constexpr (std::is_constructible_v<Ex, std::string&&>) {
    throw_exception<Ex>(what());
  } else {
    throw_exception<Ex>(what().c_str());
  }
}

template <typename Ex, typename... Args, typename Str>
[[noreturn, FOLLY_ATTR_GNU_COLD]] FOLLY_NOINLINE void
terminate_with_fmt_format_(Str str, Args... args) noexcept {
  auto what = [&] { return fmt::format(str, static_cast<Args&&>(args)...); };
  if constexpr (std::is_constructible_v<Ex, std::string&&>) {
    terminate_with<Ex>(what());
  } else {
    terminate_with<Ex>(what().c_str());
  }
}

#if FMT_VERSION >= 80000

template <typename... Args>
using fmt_format_string =
    fmt::format_string<detail::throw_exception_arg_fmt_t<Args&&>...>;

#else

template <typename...>
using fmt_format_string = fmt::string_view;

#endif

} // namespace detail

template <typename Ex, typename... Args>
[[noreturn]] FOLLY_ERASE void throw_exception_fmt_format(
    detail::fmt_format_string<Args...> str, Args&&... args) {
  detail::throw_exception_fmt_format_< //
      Ex,
      detail::throw_exception_arg_t<Args&&>...>(
      str, static_cast<Args&&>(args)...);
}

template <typename Ex, typename... Args>
[[noreturn]] FOLLY_ERASE void terminate_with_fmt_format(
    detail::fmt_format_string<Args...> str, Args&&... args) {
  detail::terminate_with_fmt_format_< //
      Ex,
      detail::throw_exception_arg_t<Args&&>...>(
      str, static_cast<Args&&>(args)...);
}

#endif

/// invoke_cold
///
/// Invoke the provided function with the provided arguments.
///
/// Usage note:
/// Passing extra values as arguments rather than capturing them allows smaller
/// inlined native code at the call-site. Passing function-pointers or function-
/// references rather than general callables with captures allows allows smaller
/// inlined native code at the call-site as well.
///
/// Example:
///
///   if (i < 0) {
///     invoke_cold(
///         [](int j) {
///           std::string ret = doStepA();
///           doStepB(ret);
///           doStepC(ret);
///         },
///         i);
///   }
template <
    typename F,
    typename... A,
    typename FD = std::remove_pointer_t<std::decay_t<F>>,
    std::enable_if_t<!std::is_function<FD>::value, int> = 0,
    typename R = decltype(FOLLY_DECLVAL(F&&)(FOLLY_DECLVAL(A&&)...))>
[[FOLLY_ATTR_GNU_COLD]] FOLLY_NOINLINE R invoke_cold(F&& f, A&&... a) //
    noexcept(noexcept(static_cast<F&&>(f)(static_cast<A&&>(a)...))) {
  return static_cast<F&&>(f)(static_cast<A&&>(a)...);
}
template <
    typename F,
    typename... A,
    typename FD = std::remove_pointer_t<std::decay_t<F>>,
    std::enable_if_t<std::is_function<FD>::value, int> = 0,
    typename R = decltype(FOLLY_DECLVAL(F&&)(FOLLY_DECLVAL(A&&)...))>
FOLLY_ERASE R invoke_cold(F&& f, A&&... a) //
    noexcept(noexcept(f(static_cast<A&&>(a)...))) {
  return f(static_cast<A&&>(a)...);
}

/// invoke_noreturn_cold
///
/// Invoke the provided function with the provided arguments. If the invocation
/// returns, terminate.
///
/// May be used with throw_exception in cases where construction of the object
/// to be thrown requires more than just invoking its constructor with a given
/// sequence of arguments passed by reference - for example, if a string message
/// must be computed before being passed to the constructor of the object to be
/// thrown.
///
/// Usage note:
/// Passing extra values as arguments rather than capturing them allows smaller
/// inlined native code at the call-site.
///
/// Example:
///
///   if (i < 0) {
///     invoke_noreturn_cold(
///         [](int j) {
///           throw_exceptions(runtime_error(to<string>("invalid: ", j)));
///         },
///         i);
///   }
template <typename F, typename... A>
[[noreturn, FOLLY_ATTR_GNU_COLD]] FOLLY_NOINLINE void
invoke_noreturn_cold(F&& f, A&&... a) noexcept(
    /* formatting */ noexcept(static_cast<F&&>(f)(static_cast<A&&>(a)...))) {
  static_cast<F&&>(f)(static_cast<A&&>(a)...);
  std::terminate();
}

/// catch_exception
///
/// Invokes t; if exceptions are enabled (if not compiled with -fno-exceptions),
/// catches a thrown exception e of type E and invokes c, forwarding e and any
/// trailing arguments.
///
/// Usage note:
/// As a general rule, pass Ex const& rather than unqualified Ex as the explicit
/// template argument E. The catch statement catches E without qualifiers so
/// if E is Ex then that translates to catch (Ex), but if E is Ex const& then
/// that translates to catch (Ex const&).
///
/// Usage note:
/// Passing extra values as arguments rather than capturing them allows smaller
/// inlined native code at the call-site.
///
/// Example:
///
///  int input = // ...
///  int def = 45;
///  auto result = catch_exception<std::runtime_error const&>(
///      [=] {
///        if (input < 0) throw std::runtime_error("foo");
///        return input;
///      },
///      [](auto&& e, int num) { return num; },
///      def);
///  assert(result == input < 0 ? def : input);
template <
    typename E,
    typename Try,
    typename Catch,
    typename... CatchA,
    typename R = std::common_type_t<
        decltype(FOLLY_DECLVAL(Try&&)()),
        decltype(FOLLY_DECLVAL(Catch&&)(
            FOLLY_DECLVAL(E&), FOLLY_DECLVAL(CatchA&&)...))>>
FOLLY_ERASE_TRYCATCH R catch_exception(Try&& t, Catch&& c, CatchA&&... a) {
#if FOLLY_HAS_EXCEPTIONS
  try {
    return static_cast<Try&&>(t)();
  } catch (E e) {
    return invoke_cold(static_cast<Catch&&>(c), e, static_cast<CatchA&&>(a)...);
  }
#else
  [](auto&&...) {}(c, a...); // ignore
  return static_cast<Try&&>(t)();
#endif
}

/// catch_exception
///
/// Invokes t; if exceptions are enabled (if not compiled with -fno-exceptions),
/// catches a thrown exception of any type and invokes c, forwarding any
/// trailing arguments.
//
/// Usage note:
/// Passing extra values as arguments rather than capturing them allows smaller
/// inlined native code at the call-site.
///
/// Example:
///
///  int input = // ...
///  int def = 45;
///  auto result = catch_exception(
///      [=] {
///        if (input < 0) throw 11;
///        return input;
///      },
///      [](int num) { return num; },
///      def);
///  assert(result == input < 0 ? def : input);
template <
    typename Try,
    typename Catch,
    typename... CatchA,
    typename R = std::common_type_t<
        decltype(FOLLY_DECLVAL(Try&&)()),
        decltype(FOLLY_DECLVAL(Catch&&)(FOLLY_DECLVAL(CatchA&&)...))>>
FOLLY_ERASE_TRYCATCH R
catch_exception(Try&& t, Catch&& c, CatchA&&... a) noexcept(
    noexcept(static_cast<Catch&&>(c)(static_cast<CatchA&&>(a)...))) {
#if FOLLY_HAS_EXCEPTIONS
  try {
    return static_cast<Try&&>(t)();
  } catch (...) {
    return invoke_cold(static_cast<Catch&&>(c), static_cast<CatchA&&>(a)...);
  }
#else
  [](auto&&...) {}(c, a...); // ignore
  return static_cast<Try&&>(t)();
#endif
}

/// rethrow_current_exception
///
/// Equivalent to:
///
///   throw;
[[noreturn]] FOLLY_ERASE void rethrow_current_exception() {
#if FOLLY_HAS_EXCEPTIONS
  throw;
#else
  std::terminate();
#endif
}

namespace detail {

unsigned int* uncaught_exceptions_ptr() noexcept;

} // namespace detail

/// uncaught_exceptions
///
/// An accelerated version of std::uncaught_exceptions.
///
/// mimic: std::uncaught_exceptions, c++17
[[FOLLY_ATTR_GNU_PURE]] FOLLY_EXPORT FOLLY_ALWAYS_INLINE int
uncaught_exceptions() noexcept {
#if defined(__APPLE__)
  return std::uncaught_exceptions();
#elif defined(_CPPLIB_VER)
  return std::uncaught_exceptions();
#elif defined(__EMSCRIPTEN__)
  return std::uncaught_exceptions();
#elif defined(__has_feature) && !FOLLY_HAS_FEATURE(cxx_thread_local)
  return std::uncaught_exceptions();
#else
  thread_local unsigned int* ct;
  return to_signed(
      FOLLY_LIKELY(!!ct) ? *ct : *(ct = detail::uncaught_exceptions_ptr()));
#endif
}

/// current_exception
///
/// An accelerated version of std::current_exception.
///
/// mimic: std::current_exception, c++11
std::exception_ptr current_exception() noexcept;

namespace detail {
#if FOLLY_APPLE_IOS
#if __IPHONE_OS_VERSION_MIN_REQUIRED < __IPHONE_12_0
inline constexpr bool exception_ptr_access_ct = false;
#else
inline constexpr bool exception_ptr_access_ct = true;
#endif
#else
inline constexpr bool exception_ptr_access_ct = true;
#endif

// 0 unknown, 1 true, -1 false
extern std::atomic<int> exception_ptr_access_rt_cache_;

[[FOLLY_ATTR_GNU_COLD]] bool exception_ptr_access_rt_v_() noexcept;
[[FOLLY_ATTR_GNU_COLD]] bool exception_ptr_access_rt_() noexcept;

inline bool exception_ptr_access_rt() noexcept {
  auto const& cache = exception_ptr_access_rt_cache_;
  auto const value = cache.load(std::memory_order_relaxed);
  return FOLLY_LIKELY(value) ? value > 0 : exception_ptr_access_rt_();
}

inline std::nullptr_t exception_ptr_nullptr() {
  return nullptr;
}

template <typename T, typename Catch>
auto exception_ptr_catching(std::exception_ptr const& ptr, Catch catch_) {
  auto const try_ = [&] {
    return ptr ? (std::rethrow_exception(ptr), nullptr) : nullptr;
  };
  return catch_exception(
      [&] { return catch_exception<T>(try_, catch_); }, exception_ptr_nullptr);
}

std::type_info const* exception_ptr_exception_typeid(
    std::exception const&) noexcept;

std::type_info const* exception_ptr_get_type_(
    std::exception_ptr const& ptr) noexcept;

void* exception_ptr_get_object_(
    std::exception_ptr const&, std::type_info const*) noexcept;

} // namespace detail

//  exception_ptr_access
//
//  Whether exception_ptr_get_type and template exception_ptr_get_object always
//  return the type or object or only do so when the stored object is of some
//  concrete type inheriting std::exception, and whether the non non-template
//  overloads of exception_ptr_get_object works at all.
//
//  Non-authoritative. For some known platforms, inspection of exception-ptr
//  objects fails. This is likely to do with mismatch between the application
//  ABI and the system-provided libstdc++/libc++/cxxabi ABI. May falsely return
//  true on other platforms.
[[FOLLY_ATTR_GNU_PURE]] inline bool exception_ptr_access() noexcept {
  return detail::exception_ptr_access_ct || detail::exception_ptr_access_rt();
}

//  exception_ptr_get_type
//
//  Returns the true runtime type info of the exception as stored.
inline std::type_info const* exception_ptr_get_type(
    std::exception_ptr const& ptr) noexcept {
  if (!exception_ptr_access()) {
    return detail::exception_ptr_catching<std::exception&>(
        ptr, detail::exception_ptr_exception_typeid);
  }
  return detail::exception_ptr_get_type_(ptr);
}

//  exception_ptr_get_object
//
//  Returns the address of the stored exception as if it were upcast to the
//  given type, if it could be upcast to that type. If no type is passed,
//  returns the address of the stored exception without upcasting.
//
//  Note that the stored exception is always a copy of the thrown exception, and
//  on some platforms caught exceptions may be copied from the stored exception.
//  The address is only the address of the object as stored, not as thrown and
//  not as caught.
inline void* exception_ptr_get_object(
    std::exception_ptr const& ptr,
    std::type_info const* const target) noexcept {
  FOLLY_SAFE_CHECK(exception_ptr_access(), "unsupported");
  return detail::exception_ptr_get_object_(ptr, target);
}

//  exception_ptr_get_object
//
//  Returns the true address of the exception as stored without upcasting.
inline void* exception_ptr_get_object( //
    std::exception_ptr const& ptr) noexcept {
  return exception_ptr_get_object(ptr, nullptr);
}

//  exception_ptr_get_object
//
//  Returns the address of the stored exception as if it were upcast to the
//  given type, if it could be upcast to that type.
template <typename T>
T* exception_ptr_get_object(std::exception_ptr const& ptr) noexcept {
  static_assert(!std::is_reference<T>::value, "is a reference");
  if (!exception_ptr_access()) {
    return detail::exception_ptr_catching<T&>(
        ptr, +[](T& ex) { return std::addressof(ex); });
  }
  auto const target = type_info_of<T>();
  auto const object =
      !to_bool(target) ? nullptr : exception_ptr_get_object(ptr, target);
  return static_cast<T*>(object);
}

//  exception_ptr_use_count
//
//  Returns the reference count of the stored exception.
//
//  Returns 0 for an empty exception_ptr. Otherwise, returns the number of
//  exception_ptr instances that refer to the same stored exception object.
//
//  Analogous to std::shared_ptr::use_count.
std::size_t exception_ptr_use_count(std::exception_ptr const& ptr) noexcept;

//  exception_ptr_unique
//
//  Returns whether the stored exception is uniquely referenced.
//
//  Returns false for an empty exception_ptr. Otherwise, returns true if this
//  is the only exception_ptr instance referring to the stored exception object.
//
//  Analogous to std::shared_ptr::unique.
bool exception_ptr_unique(std::exception_ptr const& ptr) noexcept;

/// exception_ptr_try_get_object_exact_fast
///
/// Returns the address of the stored exception as if it were upcast to the
/// given type, if its concrete type is exactly equal to one of the types passed
/// in the tag.
///
/// May hypothetically fail in cases where multipe type-info objects exist for
/// any of the given types. Positives are true but negatives may be either true
/// or false.
template <typename T, typename... S>
T* exception_ptr_try_get_object_exact_fast(
    std::exception_ptr const& ptr, tag_t<S...> /*unused*/) noexcept {
  static_assert((std::is_convertible_v<S*, T*> && ...));
  if (!kHasRtti || !ptr || !exception_ptr_access()) {
    return nullptr;
  }
  auto const type = exception_ptr_get_type(ptr);
  if (!type) {
    return nullptr;
  }
  auto const object = exception_ptr_get_object(ptr);
  auto const fun = [&](auto const phantom, std::type_info const* const target) {
    assume(!!object);
    return type == target ? static_cast<decltype(phantom)>(object) : nullptr;
  };
  T* out = nullptr;
  ((out = fun(static_cast<S*>(nullptr), FOLLY_TYPE_INFO_OF(S))) || ...);
  return out;
}

namespace detail {
template <typename T>
using detect_folly_get_exception_hint_types =
    typename std::remove_cv_t<T>::folly_get_exception_hint_types;
} // namespace detail

/// exception_ptr_get_object_hint
///
/// Returns the address of the stored exception as if it were upcast to the
/// given type, if it could be upcast to that type.
///
/// If its concrete type is exactly equal to one of the types passed in the tag,
/// this may be faster than `exception_ptr_get_object` without the hint.
///
/// Prefer the next overload that uses `T::folly_get_exception_hint_types`.
template <typename T, typename... S>
T* exception_ptr_get_object_hint(
    std::exception_ptr const& ptr, tag_t<S...> const hint) noexcept {
  auto const val = exception_ptr_try_get_object_exact_fast<T>(ptr, hint);
  return FOLLY_LIKELY(!!val) ? val : exception_ptr_get_object<T>(ptr);
}

template <typename T>
T* exception_ptr_get_object_hint(std::exception_ptr const& ptr) noexcept {
  using hints =
      detected_or_t<tag_t<T>, detail::detect_folly_get_exception_hint_types, T>;
  return exception_ptr_get_object_hint<T>(ptr, hints{});
}

/// get_exception_tag_t
///
/// A type that may contain an exception may take this passkey in the following
/// member functions:
///   - `get_exception<Ex>(get_exception_tag_t) const` when implementing the
///     `folly::get_exception<Ex>()` protocol.
///   - `get_mutable_exception<Ex>(get_exception_tag_t)` when implementing the
///     `folly::get_mutable_exception<Ex>()` protocol.
struct get_exception_tag_t {};

/// get_exception_fn
/// get_exception
/// get_mutable_exception_fn
/// get_mutable_exception
///
/// `get_exception<Ex>(v)` is meant to become the default way for accessing
/// exception-containers in `folly`.
///
///   if (auto ex = get_exception<MyError>(eptr)) {
///     LOG(INFO) << ex->myErrorDetails();
///   }
///
/// NB: Above, `ex` quacks like `MyError*`, but in some cases may be a proxy
/// class type with `->`.  Avoid having to know by writing `auto`, not `auto*`.
///
/// For the less-common scenario where you need mutable access to an error, use
/// `get_mutable_exception<Ex>(v)`.  This is a separate verb because:
///    - Mutable exception access is rare.  It may run into thread-safety bugs
///      if a `std::current_exception()` pointer is accessed outside of the
///      thread that threw it -- the standard permits reference semantics here!
///    - Making mutable access explicit enables no-alloctions, no-atomics
///      optimizations for the `const`-access path.
///
/// ## Implementing this protocol
///
/// Any exception container may provide these verbs via member functions
/// `get_exception` and `get_mutable_exception` taking `get_exception_tag_t`.
/// For an example, see `ExceptionWrapper.h`.  Requirements:
///   - Return a bare pointer to `Ex`, or a pointer-like with:
///     - `operator bool` to test if an exception was found,
///     - `operator*` and `operator->` to access the exception object,
///       respecting the const-qualification of `Ex`.
///   - Returns null if the container is not in an "error" state.
///   - Returns null if the container has an error incompatible with `Ex`.
///   - Returns a `Ex` pointer-like into the container, if it holds an error
///     whose type `From` permits `std::is_convertible<From*, Ex*>`,
///   - `nullptr` for errors incompatible with `Ex*`.
///   - `const` access via `get_exception`, mutable via `get_mutable_exception`.
///   - `noexcept`
///
/// This header provides `get_exception` support for `std::exception_ptr`.
///
/// ## Design notes
///
/// Lookup is more efficient if `Ex` matches the exact stored type, or if the
/// type alias `Ex::folly_get_exception_hint_types` provides a correct hint.
///
/// NB: `result<T>` supports `get_exception<Ex>(res)`, but `Try<T>` currently
/// omits `get_exception(get_exception_tag_t)`, because that might encourage
/// "empty state" bugs:
///
///   if (auto ex = get_exception<MyError>(tryData)) {
///     // handle error
///   } else { // Should've checked `hasValue()`!
///     doStuff(tryData.value()); // Oops, may throw `UsingUninitializedTry`!
///   }
///
/// The "lifetimebound" attribute provides _some_ use-after-free protection,
/// see the `#if 0` manual test in `get_exception_from_std_exception_ptr`.
template <typename Ex>
class get_exception_fn {
 public:
  template <typename Src>
  constexpr auto operator()(
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] const Src& src) const noexcept {
    if constexpr (std::is_same_v<Src, std::exception_ptr>) {
      return exception_ptr_get_object_hint<const Ex>(src);
    } else {
      constexpr get_exception_tag_t passkey;
      static_assert( // Return type & `noexcept`ness must match
          std::is_same_v<
              const Ex&,
              decltype(*src.template get_exception<Ex>(passkey))> &&
          noexcept(noexcept(*src.template get_exception<Ex>(passkey))));
      return src.template get_exception<Ex>(passkey);
    }
  }
  // For a mutable ptr, use `folly::get_mutable_exception<Ex>(v)` instead.
  template <typename Src>
  constexpr auto operator()(
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] Src& s) const noexcept {
    return operator()(std::as_const(s));
  }

  // It is unsafe to use `get_exception()` to get a pointer into an rvalue.
  // If you know what you're doing, add a `static_cast`.
  template <typename Src>
  void operator()(Src&&) const noexcept = delete;
  template <typename Src>
  void operator()(const Src&&) const noexcept = delete;
};
template <typename Ex>
class get_mutable_exception_fn {
 public:
  template <typename Src>
  auto operator()([[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] Src& src) const noexcept {
    if constexpr (std::is_same_v<Src, std::exception_ptr>) {
      return exception_ptr_get_object_hint<Ex>(src);
    } else {
      constexpr get_exception_tag_t passkey;
      static_assert( // Return type & `noexcept`ness must match
          std::is_same_v<
              Ex&,
              decltype(*src.template get_mutable_exception<Ex>(passkey))> &&
          noexcept(noexcept(*src.template get_mutable_exception<Ex>(passkey))));
      return src.template get_mutable_exception<Ex>(passkey);
    }
  }
  // You want `folly::get_exception<Ex>(v)` instead.
  template <typename Src>
  void operator()(const Src&) const noexcept = delete;

  // It is unsafe to use `get_mutable_exception()` to get a pointer into an
  // rvalue.  If you know what you're doing, add a `static_cast`.
  template <typename Src>
  void operator()(Src&&) const noexcept = delete;
  template <typename Src>
  void operator()(const Src&&) const noexcept = delete;
};
template <typename Ex = std::exception>
inline constexpr get_exception_fn<Ex> get_exception{};
template <typename Ex = std::exception>
inline constexpr get_mutable_exception_fn<Ex> get_mutable_exception{};

class rich_error_base;
class rich_error_code_query;

namespace detail {

// The template declaration in `rich_error_code.h` explains this setup, where
// neither of the headers includes the other.
//
// CRITICALLY IMPORTANT: Do not add or change specializations of this template
// without reading the corresponding docblock in `rich_error_code.h`, which
// explains the ODR risk inherent in this setup, and why it is currently safe.
template <typename T, typename /*SFINAE*/>
struct get_rich_error_code_traits;
template <typename T>
struct get_rich_error_code_traits<
    T,
    std::enable_if_t<
        !std::is_same_v<T, std::exception_ptr> &&
        !std::is_base_of_v<rich_error_base, T>>> {
  static constexpr void retrieve_code(
      const T& container, rich_error_code_query& query) {
    if (auto ex = folly::get_exception<folly::rich_error_base>(container)) {
      ex->retrieve_code(query);
    }
  }
};

// The libc++ and cpplib implementations do not have a move constructor or a
// move-assignment operator. To avoid refcount operations, we must improvise.
// The libstdc++ implementation has a move constructor and a move-assignment
// operator but having this does no harm.
inline std::exception_ptr extract_exception_ptr(
    std::exception_ptr&& ptr) noexcept {
  constexpr auto sz = sizeof(std::exception_ptr);
  // assume relocatability on all platforms
  // assume nrvo for performance
  std::exception_ptr ret;
  std::memcpy(static_cast<void*>(&ret), &ptr, sz);
  std::memset(static_cast<void*>(&ptr), 0, sz);
  return ret;
}

struct make_exception_ptr_with_arg_ {
  using dtor_ret_t = std::conditional_t<kIsArchWasm, void*, void>;

  size_t size = 0;
  std::type_info const* type = nullptr;
  void (*ctor)(void*, void*) = nullptr;
  dtor_ret_t (*dtor)(void*) = nullptr;

  template <typename F, typename E>
  static void make(void* p, void* f) {
    ::new (p) E((*static_cast<F*>(f))());
  }

  template <typename E>
  static dtor_ret_t dtor_(void* ptr) {
    static_cast<E*>(ptr)->~E();
    return dtor_ret_t(ptr);
  }

  template <typename F, typename E = decltype(FOLLY_DECLVAL(F&)())>
  FOLLY_ERASE explicit constexpr make_exception_ptr_with_arg_(
      tag_t<F> /*unused*/) noexcept
      : size{sizeof(E)},
        type{FOLLY_TYPE_INFO_OF(E)},
        ctor{make<F, E>},
        dtor{dtor_<E>} {}
};

std::exception_ptr make_exception_ptr_with_(
    make_exception_ptr_with_arg_ const&, void*) noexcept;

template <typename F>
struct make_exception_ptr_with_fn_ {
  F& f_;
  FOLLY_ERASE std::exception_ptr operator()() const {
    return std::make_exception_ptr(f_());
  }
};

} // namespace detail

/// make_exception_ptr_with_fn
/// make_exception_ptr_with
///
/// Constructs a std::exception_ptr. On some platforms, this form may be more
/// efficient than std::make_exception_ptr. In particular, even when the latter
/// is optimized not actually to throw, catch, and call std::current_exception
/// internally, it remains specified to take its parameter by-value and to copy
/// its parameter internally. Many in-practice exception types, including those
/// which ship with standard libraries implementations, have copy constructors
/// which may atomically modify refcounts; others may allocate and copy string
/// data. In the best-case scenario, folly::make_exception_ptr_with may avoid
/// these costs.
//
/// There are three overloads, with overload selection unambiguous.
/// * A single invocable argument. The argument is invoked and its return value
///   is the managed exception.
/// * Variadic arguments, the first of which is in_place_type<E>. An exception
///   of type E is created in-place with the remaining arguments forwarded to
///   the constructor of E, and it is the managed exception.
/// * Two arguments, the first of which is in_place. The argument is moved or
///   copied and the result is the managed exception. This form is the closest
///   to std::make_exception_ptr.
///
/// Example:
///
///   std::exception_ptr eptr = make_exception_ptr_with(
///       [] { return std::runtime_error("message string"); });
///
///   std::exception_ptr eptr = make_exception_ptr_with(
///       std::in_place_type<std::runtime_error>, "message string");
///
///   std::exception_ptr eptr = make_exception_ptr_with(
///       std::in_place, std::runtime_error("message string");
///
/// In each example above, the variable eptr holds a managed exception object of
/// type std::runtime_error with a message string "message string" that would be
/// returned by member what().
///
/// Note that a managed exception object can have any value type whatsoever; it
/// is not required to have value type of or inheriting std::exception. This is
/// the same principle as for throw statements and throw_exception above.
struct make_exception_ptr_with_fn {
 private:
  template <typename R>
  using make_arg_ = conditional_t<
      std::is_array<std::remove_reference_t<R>>::value,
      detail::throw_exception_arg_array_,
      detail::throw_exception_arg_base_>;
  template <typename R>
  using make_arg_t = typename make_arg_<R>::template apply<R>;

  template <typename E, typename... A>
  auto make(A&&... a) const noexcept {
    return [&] { return E(static_cast<A&&>(a)...); };
  }

 public:
  template <typename F, decltype(FOLLY_DECLVAL(F&)())* = nullptr>
  std::exception_ptr operator()(F f) const noexcept {
    if ((kIsGlibcxx || kIsLibcpp) && !kIsApple && !kIsWindows //
        && kHasRtti && exception_ptr_access()) {
      static const detail::make_exception_ptr_with_arg_ arg{tag<F>};
      return detail::make_exception_ptr_with_(arg, &f);
    }
    if (kHasExceptions) {
      return catch_exception(
          detail::make_exception_ptr_with_fn_<F>{f}, current_exception);
    }
    return std::exception_ptr();
  }
  template <typename E, typename... A>
  FOLLY_ERASE std::exception_ptr operator()(
      std::in_place_type_t<E> /*unused*/, A&&... a) const noexcept {
    return operator()(make<E, make_arg_t<A&&>...>(static_cast<A&&>(a)...));
  }
  template <typename E>
  FOLLY_ERASE std::exception_ptr operator()(
      std::in_place_t /*unused*/, E&& e) const noexcept {
    constexpr auto tag = std::in_place_type<std::remove_cvref_t<E>>;
    check_(FOLLY_TYPE_INFO_OF(std::decay_t<E>), FOLLY_TYPE_INFO_OF(e));
    return operator()(tag, static_cast<E&&>(e));
  }

 private:
  FOLLY_ALWAYS_INLINE void check_(
      std::type_info const* s, std::type_info const* d) const noexcept {
    FOLLY_SAFE_DCHECK(
        !s || !d || *s == *d,
        "mismatched static and dynamic types indicates object slicing");
  }
};
inline constexpr make_exception_ptr_with_fn make_exception_ptr_with{};

//  exception_shared_string
//
//  An immutable refcounted string, with the same layout as a pointer, suitable
//  for use in an exception. Exceptions are intended to cheaply nothrow-copy-
//  constructible and mostly do not need to optimize moves, and this affects how
//  exception messages are best stored.
//
//  May be constructed with a string literal pointer, which will be stored with
//  no refcount required.
class exception_shared_string {
 private:
  using format_sig_ = void(void*, char*, std::size_t);

  template <typename F>
  using test_format_ =
      decltype(FOLLY_DECLVAL(F)(static_cast<char*>(nullptr), std::size_t(0)));

  static void test_params_(char const*, std::size_t);
  template <typename F>
  static void ffun_(void* f, char* b, std::size_t l) {
    (*static_cast<F*>(f))(b, l);
  }

  struct state; // alignment is alignof(void*)

  struct tagged_what_t {
    static inline constexpr uintptr_t non_literal_mask = uintptr_t(1)
        << (sizeof(const char*) * 8 - 1);

    //  The top bit of p_ encodes where the string lives:
    //  - top bit == 0: in an immortal literal
    //  - top bit == 1: in an refcounted allocated state
    //  The top bit of a userspace pointer is zero on all supported platforms.
    const char* p_;

    void assert_top_bit_is_zero(const char* p) {
      // Debug-only on 64-bit platforms because all known ones leave the top
      // bit free.  Userspace pointers MAY use the top bit on unsupported
      // 32-bit platforms -- abort in opt builds, instead of corrupting memory.
      //
      // Limitations of constexpr force a gap in assertion coverage -- if a
      // literal const char* sets the top bit (seems very unlikely!), and it is
      // only used in a constexpr exception_shared_string, then invalid memory
      // access would be triggered by what(), and the copy constructor.
      if constexpr (sizeof(void*) != 8 || kIsDebug) {
        FOLLY_SAFE_CHECK(!(uintptr_t(p) & non_literal_mask));
      }
    }

#if !defined(__NVCC__)
    constexpr tagged_what_t(vtag_t<true> /*literal*/, const char* p) : p_{p} {
      if (!std::is_constant_evaluated()) {
        assert_top_bit_is_zero(p);
      }
    }
#endif
    // NEVER make this `constexpr`; `what()` explains why.
    tagged_what_t(vtag_t<false> /*allocated*/, const char* p)
        : p_{reinterpret_cast<const char*>(
              non_literal_mask | reinterpret_cast<uintptr_t>(p))} {
      assert_top_bit_is_zero(p);
    }

    bool is_literal() const noexcept {
      return !(non_literal_mask & reinterpret_cast<uintptr_t>(p_));
    }
    constexpr const char* what() const noexcept {
      if (std::is_constant_evaluated()) {
        // Only the literal ctor is `constexpr`, so we can assume no
        // bit-twiddling is needed.  Consteval code cannot do Pointer bithacks.
        return p_;
      }
      return reinterpret_cast<const char*>(
          ~non_literal_mask & reinterpret_cast<uintptr_t>(p_));
    }
  };
  static_assert(sizeof(tagged_what_t) == sizeof(void*));

  tagged_what_t tagged_what_;

  exception_shared_string(std::size_t, format_sig_&, void*);

  static char const* from_state(state const* state) noexcept;
  static state* to_state(const tagged_what_t&) noexcept;
  void ruin_state() noexcept;

 public:
#if !defined(__NVCC__)
  constexpr explicit exception_shared_string(literal_c_str p) noexcept
      : tagged_what_{vtag<true>, p.ptr} {}
#endif

  exception_shared_string(char const*, std::size_t);

  template <
      typename String,
      typename = decltype(test_params_(
          FOLLY_DECLVAL(String const&).data(),
          FOLLY_DECLVAL(String const&).size()))>
  explicit exception_shared_string(String const& str)
      : exception_shared_string{str.data(), str.size()} {}

  template <typename F, decltype((void(test_format_<F&>()), 0)) = 0>
  exception_shared_string(std::size_t size, F func)
      : exception_shared_string(
            size, ffun_<F>, &reinterpret_cast<unsigned char&>(func)) {}

  exception_shared_string(exception_shared_string const&) noexcept;
  exception_shared_string& operator=(exception_shared_string const&) noexcept;

#if !defined(__NVCC__)
  constexpr exception_shared_string(exception_shared_string&& that) noexcept
      : tagged_what_{that.tagged_what_} {
    that.tagged_what_ =
        tagged_what_t{vtag<true>, ""}; // safe-to-read moved-out state
  }

  exception_shared_string& operator=(exception_shared_string&&) noexcept;
#else
  exception_shared_string(exception_shared_string&&) = delete;
  exception_shared_string& operator=(exception_shared_string&&) = delete;
#endif

  constexpr ~exception_shared_string() {
    if (!std::is_constant_evaluated()) {
      ruin_state();
    }
  }

  constexpr char const* what() const noexcept { return tagged_what_.what(); }
};

} // namespace folly
