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

#include <cassert>
#include <cstdint>
#include <exception>
#include <iosfwd>
#include <memory>
#include <new>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <fmt/format.h>

#include <folly/CPortability.h>
#include <folly/CppAttributes.h>
#include <folly/Demangle.h>
#include <folly/ExceptionString.h>
#include <folly/FBString.h>
#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/functional/traits.h>
#include <folly/lang/Assume.h>
#include <folly/lang/Exception.h>

namespace folly {

//! Throwing exceptions can be a convenient way to handle errors. Storing
//! exceptions in an `exception_ptr` makes it easy to handle exceptions in a
//! different thread or at a later time. `exception_ptr` can also be used in a
//! very generic result/exception wrapper.
//!
//! However, inspecting exceptions through the `exception_ptr` interface, namely
//! through `rethrow_exception`, is expensive. This is a wrapper interface which
//! offers faster inspection.
//!
//! \par Example usage:
//! \code
//! exception_wrapper globalExceptionWrapper;
//!
//! // Thread1
//! void doSomethingCrazy() {
//!   int rc = doSomethingCrazyWithLameReturnCodes();
//!   if (rc == NAILED_IT) {
//!     globalExceptionWrapper = exception_wrapper();
//!   } else if (rc == FACE_PLANT) {
//!     globalExceptionWrapper = make_exception_wrapper<FacePlantException>();
//!   } else if (rc == FAIL_WHALE) {
//!     globalExceptionWrapper = make_exception_wrapper<FailWhaleException>();
//!   }
//! }
//!
//! // Thread2: Exceptions are ok!
//! void processResult() {
//!   try {
//!     globalExceptionWrapper.throw_exception();
//!   } catch (const FacePlantException& e) {
//!     LOG(ERROR) << "FACEPLANT!";
//!   } catch (const FailWhaleException& e) {
//!     LOG(ERROR) << "FAILWHALE!";
//!   }
//! }
//!
//! // Thread2: Exceptions are bad!
//! void processResult() {
//!   globalExceptionWrapper.handle(
//!       [&](FacePlantException& faceplant) {
//!         LOG(ERROR) << "FACEPLANT";
//!       },
//!       [&](FailWhaleException& failwhale) {
//!         LOG(ERROR) << "FAILWHALE!";
//!       },
//!       [](...) {
//!         LOG(FATAL) << "Unrecognized exception";
//!       });
//! }
//! \endcode
class exception_wrapper final {
 private:
  struct with_exception_from_fn_;
  struct with_exception_from_ex_;

  [[noreturn]] static void onNoExceptionError(char const* name);

  template <class Ex>
  using IsStdException = std::is_base_of<std::exception, std::decay_t<Ex>>;

  std::exception_ptr ptr_;

  template <class T>
  struct IsRegularExceptionType
      : StrictConjunction<
            std::is_copy_constructible<T>,
            std::negation<std::is_base_of<exception_wrapper, T>>,
            std::negation<std::is_abstract<T>>> {};

  template <class This, class Fn>
  static bool with_exception_(This& this_, Fn fn_, tag_t<void>);

  template <class This, class Fn, typename Ex>
  static bool with_exception_(This& this_, Fn fn_, tag_t<Ex>);

  template <class Ex, class This, class Fn>
  static bool with_exception_(This& this_, Fn fn_);

  template <class This, class... CatchFns>
  static void handle_(This& this_, char const* name, CatchFns&... fns);

 public:
  //! Default-constructs an empty `exception_wrapper`
  //! \post `type() == nullptr`
  exception_wrapper() noexcept {}

  //! Move-constructs an `exception_wrapper`
  //! \post `*this` contains the value of `that` prior to the move
  //! \post `that.type() == nullptr`
  exception_wrapper(exception_wrapper&& that) noexcept;

  //! Copy-constructs an `exception_wrapper`
  //! \post `*this` contains a copy of `that`, and `that` is unmodified
  //! \post `type() == that.type()`
  exception_wrapper(exception_wrapper const& that) = default;

  //! Move-assigns an `exception_wrapper`
  //! \pre `this != &that`
  //! \post `*this` contains the value of `that` prior to the move
  //! \post `that.type() == nullptr`
  exception_wrapper& operator=(exception_wrapper&& that) noexcept;

  //! Copy-assigns an `exception_wrapper`
  //! \post `*this` contains a copy of `that`, and `that` is unmodified
  //! \post `type() == that.type()`
  exception_wrapper& operator=(exception_wrapper const& that) = default;

  //! \post `!ptr || bool(*this)`
  explicit exception_wrapper(std::exception_ptr const& ptr) noexcept;
  explicit exception_wrapper(std::exception_ptr&& ptr) noexcept;

  //! \pre `typeid(ex) == typeid(typename decay<Ex>::type)`
  //! \post `bool(*this)`
  //! \post `type() == &typeid(ex)`
  //! \note Exceptions of types derived from `std::exception` can be implicitly
  //!     converted to an `exception_wrapper`.
  //! \note This uses SFINAE rather than a `requires`-clause: checking whether
  //!     an exception type constructible from `exception_wrapper` is copyable
  //!     recurses into this constructor, which SFINAE tolerates but constraint
  //!     satisfaction rejects.
  template <
      class Ex,
      class Ex_ = std::decay_t<Ex>,
      std::enable_if_t<
          std::conjunction_v<IsStdException<Ex_>, IsRegularExceptionType<Ex_>>,
          int> = 0>
  /* implicit */ exception_wrapper(Ex&& ex);

  //! \pre `typeid(ex) == typeid(typename decay<Ex>::type)`
  //! \post `bool(*this)`
  //! \post `type() == &typeid(ex)`
  //! \note Exceptions of types not derived from `std::exception` can still be
  //!     used to construct an `exception_wrapper`, but you must specify
  //!     `std::in_place` as the first parameter.
  template <class Ex>
  exception_wrapper(std::in_place_t, Ex&& ex)
    requires(IsRegularExceptionType<std::decay_t<Ex>>::value);

  template <class Ex, typename... As>
  exception_wrapper(std::in_place_type_t<Ex>, As&&... as)
    requires(IsRegularExceptionType<Ex>::value);

  //! Swaps the value of `*this` with the value of `that`
  void swap(exception_wrapper& that) noexcept;

  //! \return `true` if `*this` is holding an exception.
  explicit operator bool() const noexcept;

  //! \return `!bool(*this)`
  bool operator!() const noexcept;

  //! Make this `exception_wrapper` empty
  //! \post `!*this`
  void reset();

  //! \return `true` if this `exception_wrapper` holds an exception.
  bool has_exception_ptr() const noexcept;

  //! \return a pointer to the `std::exception` held by `*this`, if it holds
  //!     one; otherwise, returns `nullptr`.
  std::exception* get_mutable_exception() const noexcept;
  std::exception const* get_exception() const noexcept;

  //! \returns a pointer to the `Ex` held by `*this`, if it holds an object
  //!     whose type `From` permits `std::is_convertible<From*, Ex*>`;
  //!     otherwise, returns `nullptr`.
  //!
  //! `folly::get_exception<Ex>(ew)` is identical, but avoids the "dependent
  //! template" wart, and supports inspecting other types.
  //!
  //! This is most efficient when `Ex` matches the exact stored type, or when
  //! the type alias `Ex::folly_get_exception_hint_types` has a good hint.
  //! \overload
  template <typename Ex>
  Ex* get_mutable_exception() const noexcept;
  template <typename Ex>
  Ex const* get_exception() const noexcept;

  //! \return A `std::exception_ptr` that references the exception held by
  //!     `*this`.
  std::exception_ptr to_exception_ptr() const& noexcept;
  // NB: Can add this back, if a good use-case arises.
  std::exception_ptr to_exception_ptr() && = delete;

  std::exception_ptr& exception_ptr() & noexcept;
  std::exception_ptr const& exception_ptr() const& noexcept;
  std::exception_ptr&& exception_ptr() && noexcept;
  std::exception_ptr const&& exception_ptr() const&& noexcept;

  //! \return `true` if the wrappers point to the same exception object
  friend inline bool operator==(
      exception_wrapper const& lhs, exception_wrapper const& rhs) noexcept {
    return lhs.ptr_ == rhs.ptr_;
  }

  //! Returns the `typeid` of the wrapped exception object. If there is no
  //!     wrapped exception object, returns `nullptr`.
  std::type_info const* type() const noexcept;

  //! \return If `get_exception() != nullptr`, `class_name() + ": " +
  //!     get_exception()->what()`; otherwise, `class_name()`.
  folly::fbstring what() const;

  //! \return If `!*this`, the empty string; otherwise, if `!type()`, text that
  //!     is not a class name; otherwise, the demangling of `type()->name()`.
  folly::fbstring class_name() const;

  //! \tparam Ex The expression type to check for compatibility with.
  //! \return `true` if and only if `*this` wraps an exception that would be
  //!     caught with a `catch(Ex const&)` clause.
  //! \note If `*this` is empty, this function returns `false`.
  template <class Ex>
  bool is_compatible_with() const noexcept;

  //! Throws the wrapped expression.
  //! \pre `bool(*this)`
  [[noreturn]] void throw_exception() const;

  //! Terminates the process with the wrapped expression.
  [[noreturn]] void terminate_with() const noexcept { throw_exception(); }

  //! Throws the wrapped expression nested into another exception.
  //! \pre `bool(*this)`
  //! \param ex Exception in *this will be thrown nested into ex;
  //      see std::throw_with_nested() for details on this semantic.
  template <class Ex>
  [[noreturn]] void throw_with_nested(Ex&& ex) const;

  //! Call `fn` with the wrapped exception (if any), if `fn` can accept it.
  //! \par Example
  //! \code
  //! exception_wrapper ew{std::runtime_error("goodbye cruel world")};
  //!
  //! assert( ew.with_exception([](std::runtime_error& e){/*...*/}) );
  //!
  //! assert( !ew.with_exception([](int& e){/*...*/}) );
  //!
  //! assert( !exception_wrapper{}.with_exception([](int& e){/*...*/}) );
  //! \endcode
  //! \tparam Ex Optionally, the type of the exception that `fn` accepts.
  //! \tparam Fn The type of a monomorphic function object.
  //! \param fn A function object to call with the wrapped exception
  //! \return `true` if and only if `fn` was called.
  //! \note Optionally, you may explicitly specify the type of the exception
  //!     that `fn` expects, as in
  //! \code
  //! ew.with_exception<std::runtime_error>([](auto&& e) { /*...*/; });
  //! \endcode
  //! \note The handler is not invoked with an active exception.
  //!     **Do not try to rethrow the exception with `throw;` from within your
  //!     handler -- that is, a throw expression with no operand.** This may
  //!     cause your process to terminate. (It is perfectly ok to throw from
  //!     a handler so long as you specify the exception to throw, as in
  //!     `throw e;`.)
  template <class Ex = void const, class Fn>
  bool with_exception(Fn fn);
  //! \overload
  template <class Ex = void const, class Fn>
  bool with_exception(Fn fn) const;

  //! Handle the wrapped expression as if with a series of `catch` clauses,
  //!     propagating the exception if no handler matches.
  //! \par Example
  //! \code
  //! exception_wrapper ew{std::runtime_error("goodbye cruel world")};
  //!
  //! ew.handle(
  //!   [&](std::logic_error const& e) {
  //!      LOG(DFATAL) << "ruh roh";
  //!      ew.throw_exception(); // rethrow the active exception without
  //!                           // slicing it. Will not be caught by other
  //!                           // handlers in this call.
  //!   },
  //!   [&](std::exception const& e) {
  //!      LOG(ERROR) << ew.what();
  //!   });
  //! \endcode
  //! In the above example, any exception _not_ derived from `std::exception`
  //!     will be propagated. To specify a catch-all clause, pass a lambda that
  //!     takes a C-style ellipses, as in:
  //! \code
  //! ew.handle(/*...* /, [](...) { /* handle unknown exception */ } )
  //! \endcode
  //! \pre `!*this`
  //! \tparam CatchFns A pack of unary monomorphic function object types.
  //! \param fns A pack of unary monomorphic function objects to be treated as
  //!     an ordered list of potential exception handlers.
  //! \note The handlers are not invoked with an active exception.
  //!     **Do not try to rethrow the exception with `throw;` from within your
  //!     handler -- that is, a throw expression with no operand.** This may
  //!     cause your process to terminate. (It is perfectly ok to throw from
  //!     a handler so long as you specify the exception to throw, as in
  //!     `throw e;`.)
  template <class... CatchFns>
  void handle(CatchFns... fns);
  //! \overload
  template <class... CatchFns>
  void handle(CatchFns... fns) const;

  // Implement the `folly::get_exception<Ex>(ew)` protocol
  template <typename Ex>
  Ex const* get_exception(get_exception_tag_t) const noexcept {
    return get_exception<Ex>();
  }
  template <typename Ex>
  Ex* get_mutable_exception(get_exception_tag_t) const noexcept {
    return get_mutable_exception<Ex>();
  }
};

/**
 * \return An `exception_wrapper` that wraps an instance of type `Ex`
 *     that has been constructed with arguments `std::forward<As>(as)...`.
 */
template <class Ex, typename... As>
exception_wrapper make_exception_wrapper(As&&... as) {
  return exception_wrapper{std::in_place_type<Ex>, std::forward<As>(as)...};
}

/**
 * Inserts `ew.what()` into the ostream `sout`.
 * \return `sout`
 */
template <class Ch>
std::basic_ostream<Ch>& operator<<(
    std::basic_ostream<Ch>& sout, exception_wrapper const& ew) {
  sout << ew.class_name();
  if (auto e = ew.get_exception()) {
    sout << ": " << e->what();
  }
  return sout;
}

/**
 * Swaps the value of `a` with the value of `b`.
 */
inline void swap(exception_wrapper& a, exception_wrapper& b) noexcept {
  a.swap(b);
}

// For consistency with exceptionStr() functions in ExceptionString.h
fbstring exceptionStr(exception_wrapper const& ew);

//! `try_and_catch` is a convenience for `try {} catch(...) {}`` that returns an
//! `exception_wrapper` with the thrown exception, if any.
template <typename F>
exception_wrapper try_and_catch(F&& fn) noexcept {
  auto x = [&] { return void(static_cast<F&&>(fn)()), std::exception_ptr{}; };
  return exception_wrapper{catch_exception(x, current_exception)};
}

/**
 * A convenience shorthand for `exception_wrapper(current_exception())`.
 */
inline exception_wrapper current_exception_wrapper() noexcept {
  return exception_wrapper{current_exception()};
}

} // namespace folly

template <>
struct fmt::formatter<folly::exception_wrapper> : formatter<folly::fbstring> {
  template <typename Context>
  auto format(const folly::exception_wrapper& ew, Context& ctx) const {
    return formatter<folly::fbstring>::format(ew.what(), ctx);
  }
};

#include <folly/ExceptionWrapper-inl.h>
