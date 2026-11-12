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

/**
 * Like folly::Optional, but can store a value *or* an error.
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <folly/CPortability.h>
#include <folly/CppAttributes.h>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/Preprocessor.h>
#include <folly/Traits.h>
#include <folly/Unit.h>
#include <folly/Utility.h>
#include <folly/lang/Exception.h>
#include <folly/lang/Hint.h>

#define FOLLY_EXPECTED_ID(X) FB_CONCATENATE(FB_CONCATENATE(Folly, X), __LINE__)

#define FOLLY_REQUIRES_IMPL(...)                                            \
  bool FOLLY_EXPECTED_ID(Requires) = false,                                 \
       typename std::enable_if<                                             \
           (FOLLY_EXPECTED_ID(Requires) || static_cast<bool>(__VA_ARGS__)), \
           int>::type = 0

#define FOLLY_REQUIRES_TRAILING(...) , FOLLY_REQUIRES_IMPL(__VA_ARGS__)

#define FOLLY_REQUIRES(...) template <FOLLY_REQUIRES_IMPL(__VA_ARGS__)>

namespace folly {

namespace expected_detail {
namespace expected_detail_ExpectedHelper {
struct ExpectedHelper;
}
/* using override */ using expected_detail_ExpectedHelper::ExpectedHelper;
} // namespace expected_detail

/**
 * Unexpected - a helper type used to disambiguate the construction of
 * Expected objects in the error state.
 */
template <class Error>
class [[nodiscard]] Unexpected final {
  template <class E>
  friend class Unexpected;
  template <class V, class E>
  friend class Expected;
  friend struct expected_detail::ExpectedHelper;

 public:
  /**
   * Constructors
   */
  Unexpected() = default;
  Unexpected(const Unexpected&) = default;
  Unexpected(Unexpected&&) = default;
  Unexpected& operator=(const Unexpected&) = default;
  Unexpected& operator=(Unexpected&&) = default;
  [[FOLLY_ATTR_GNU_COLD]] constexpr /* implicit */ Unexpected(const Error& err)
      : error_(err) {}
  [[FOLLY_ATTR_GNU_COLD]] constexpr /* implicit */ Unexpected(Error&& err)
      : error_(std::move(err)) {}

  template <class Other FOLLY_REQUIRES_TRAILING(
      std::is_constructible<Error, Other&&>::value)>
  constexpr /* implicit */ Unexpected(Unexpected<Other> that)
      : error_(std::move(that.error())) {}

  /**
   * Assignment
   */
  template <class Other FOLLY_REQUIRES_TRAILING(
      std::is_assignable<Error&, Other&&>::value)>
  Unexpected& operator=(Unexpected<Other> that) {
    error_ = std::move(that.error());
  }

  /**
   * Observers
   */
  Error& error() & noexcept { return error_; }
  const Error& error() const& noexcept { return error_; }
  Error&& error() && noexcept { return std::move(error_); }
  const Error&& error() const&& noexcept { return std::move(error_); }

 private:
  Error error_;
};

template <
    class Error FOLLY_REQUIRES_TRAILING(IsEqualityComparable<Error>::value)>
inline bool operator==(
    const Unexpected<Error>& lhs, const Unexpected<Error>& rhs) {
  return lhs.error() == rhs.error();
}

/**
 * For constructing an Unexpected object from an error code. Unexpected objects
 * are implicitly convertible to Expected object in the error state. Usage is
 * as follows:
 *
 * enum class MyErrorCode { BAD_ERROR, WORSE_ERROR };
 * Expected<int, MyErrorCode> myAPI() {
 *   int i = // ...;
 *   return i ? makeExpected<MyErrorCode>(i)
 *            : makeUnexpected(MyErrorCode::BAD_ERROR);
 * }
 */
template <class Error>
constexpr Unexpected<typename std::decay<Error>::type> makeUnexpected(
    Error&& err) {
  return Unexpected<typename std::decay<Error>::type>{
      static_cast<Error&&>(err)};
}

template <class Error>
class FOLLY_EXPORT BadExpectedAccess;

/**
 * A common base type for exceptions thrown by Expected when the caller
 * erroneously requests a value.
 */
template <>
class FOLLY_EXPORT BadExpectedAccess<void> : public std::exception {
 public:
  explicit BadExpectedAccess() noexcept = default;
  BadExpectedAccess(BadExpectedAccess const&) {}
  BadExpectedAccess& operator=(BadExpectedAccess const&) { return *this; }

  char const* what() const noexcept override { return "bad expected access"; }
};

/**
 * An exception type thrown by Expected on catastrophic logic errors, i.e., when
 * the caller tries to access the value within an Expected but when the Expected
 * instead contains an error.
 */
template <class Error>
class FOLLY_EXPORT BadExpectedAccess : public BadExpectedAccess<void> {
 public:
  explicit BadExpectedAccess(Error error)
      : error_{static_cast<Error&&>(error)} {}

  /**
   * The error code that was held by the Expected object when the caller
   * erroneously requested the value.
   */
  Error& error() & { return error_; }
  Error const& error() const& { return error_; }
  Error&& error() && { return static_cast<Error&&>(error_); }
  Error const&& error() const&& { return static_cast<Error const&&>(error_); }

 private:
  Error error_;
};

/**
 * Forward declarations
 */
template <class Value, class Error>
class Expected;

template <class Error, class Value>
[[nodiscard]] constexpr Expected<typename std::decay<Value>::type, Error>
makeExpected(Value&&);

/**
 * Alias for an Expected type's associated value_type
 */
template <class Expected>
using ExpectedValueType =
    typename std::remove_reference<Expected>::type::value_type;

/**
 * Alias for an Expected type's associated error_type
 */
template <class Expected>
using ExpectedErrorType =
    typename std::remove_reference<Expected>::type::error_type;

// Details...
namespace expected_detail {

template <typename Value, typename Error>
struct Promise;
template <typename Value, typename Error>
struct PromiseReturn;

template <template <class...> class Trait, class... Ts>
using StrictAllOf = StrictConjunction<Trait<Ts>...>;

template <class T>
using IsCopyable = StrictConjunction<
    std::is_copy_constructible<T>,
    std::is_copy_assignable<T>>;

template <class T>
using IsMovable = StrictConjunction<
    std::is_move_constructible<T>,
    std::is_move_assignable<T>>;

template <class T>
using IsNothrowCopyable = StrictConjunction<
    std::is_nothrow_copy_constructible<T>,
    std::is_nothrow_copy_assignable<T>>;

template <class T>
using IsNothrowMovable = StrictConjunction<
    std::is_nothrow_move_constructible<T>,
    std::is_nothrow_move_assignable<T>>;

template <class From, class To>
using IsConvertible = StrictConjunction<
    std::is_constructible<To, From>,
    std::is_assignable<To&, From>>;

template <class T, class U>
auto doEmplaceAssign(int, T& t, U&& u)
    -> decltype(void(t = static_cast<U&&>(u))) {
  t = static_cast<U&&>(u);
}

template <class T, class U>
auto doEmplaceAssign(long, T& t, U&& u)
    -> decltype(void(T(static_cast<U&&>(u)))) {
  auto addr = const_cast<void*>(static_cast<void const*>(std::addressof(t)));
  t.~T();
  ::new (addr) T(static_cast<U&&>(u));
}

template <class T, class... Us>
auto doEmplaceAssign(int, T& t, Us&&... us)
    -> decltype(void(t = T(static_cast<Us&&>(us)...))) {
  t = T(static_cast<Us&&>(us)...);
}

template <class T, class... Us>
auto doEmplaceAssign(long, T& t, Us&&... us)
    -> decltype(void(T(static_cast<Us&&>(us)...))) {
  auto addr = const_cast<void*>(static_cast<void const*>(std::addressof(t)));
  t.~T();
  ::new (addr) T(static_cast<Us&&>(us)...);
}

struct EmptyTag {};
struct ValueTag {};
struct ErrorTag {};
enum class Which : unsigned char { eEmpty, eValue, eError };
enum class StorageType { ePODStruct, ePODUnion, eUnion };

template <class Value, class Error>
constexpr StorageType getStorageType() {
  return StrictAllOf<std::is_trivially_copyable, Value, Error>::value
      ? (sizeof(std::pair<Value, Error>) <= sizeof(void* [2]) &&
                 StrictAllOf<std::is_trivial, Value, Error>::value
             ? StorageType::ePODStruct
             : StorageType::ePODUnion)
      : StorageType::eUnion;
}

template <
    class Value,
    class Error,
    StorageType = expected_detail::getStorageType<Value, Error>()> // ePODUnion
struct ExpectedStorage {
  using value_type = Value;
  using error_type = Error;
  union {
    Value value_;
    Error error_;
    char ch_;
  };
  Which which_;

  template <class E = Error, class = decltype(E{})>
  constexpr ExpectedStorage() noexcept(noexcept(E{}))
      : error_{}, which_(Which::eError) {}
  explicit constexpr ExpectedStorage(EmptyTag) noexcept
      : ch_{unsafe_default_initialized}, which_(Which::eEmpty) {}
  template <class... Vs>
  explicit constexpr ExpectedStorage(ValueTag, Vs&&... vs) noexcept(
      noexcept(Value(static_cast<Vs&&>(vs)...)))
      : value_(static_cast<Vs&&>(vs)...), which_(Which::eValue) {}
  template <class... Es>
  explicit constexpr ExpectedStorage(ErrorTag, Es&&... es) noexcept(
      noexcept(Error(static_cast<Es&&>(es)...)))
      : error_(static_cast<Es&&>(es)...), which_(Which::eError) {}
  void clear() noexcept {}
  static constexpr bool uninitializedByException() noexcept {
    // Although which_ may temporarily be eEmpty during construction, it
    // is always either eValue or eError for a fully-constructed Expected.
    return false;
  }
  template <class... Vs>
  void assignValue(Vs&&... vs) {
    expected_detail::doEmplaceAssign(0, value_, static_cast<Vs&&>(vs)...);
    which_ = Which::eValue;
  }
  template <class... Es>
  void assignError(Es&&... es) {
    expected_detail::doEmplaceAssign(0, error_, static_cast<Es&&>(es)...);
    which_ = Which::eError;
  }
  template <class Other>
  void assign(Other&& that) {
    switch (that.which_) {
      case Which::eValue:
        this->assignValue(static_cast<Other&&>(that).value());
        break;
      case Which::eError:
        this->assignError(static_cast<Other&&>(that).error());
        break;
      case Which::eEmpty:
      default:
        this->clear();
        break;
    }
  }
  Value& value() & { return value_; }
  const Value& value() const& { return value_; }
  Value&& value() && { return std::move(value_); }
  const Value&& value() const&& { return std::move(value_); }
  Error& error() & { return error_; }
  const Error& error() const& { return error_; }
  Error&& error() && { return std::move(error_); }
  const Error&& error() const&& { return std::move(error_); }
};

template <class Value, class Error>
struct ExpectedUnion {
  union {
    Value value_;
    Error error_;
    char ch_ = unsafe_default_initialized;
  };
  Which which_ = Which::eEmpty;

  explicit constexpr ExpectedUnion(EmptyTag) noexcept {}
  template <class... Vs>
  explicit constexpr ExpectedUnion(ValueTag, Vs&&... vs) noexcept(
      noexcept(Value(static_cast<Vs&&>(vs)...)))
      : value_(static_cast<Vs&&>(vs)...), which_(Which::eValue) {}
  template <class... Es>
  explicit constexpr ExpectedUnion(ErrorTag, Es&&... es) noexcept(
      noexcept(Error(static_cast<Es&&>(es)...)))
      : error_(static_cast<Es&&>(es)...), which_(Which::eError) {}
  ExpectedUnion(const ExpectedUnion&) {}
  ExpectedUnion(ExpectedUnion&&) noexcept {}
  ExpectedUnion& operator=(const ExpectedUnion&) { return *this; }
  ExpectedUnion& operator=(ExpectedUnion&&) noexcept { return *this; }
  ~ExpectedUnion() {}
  Value& value() & { return value_; }
  const Value& value() const& { return value_; }
  Value&& value() && { return std::move(value_); }
  const Value&& value() const&& { return std::move(value_); }
  Error& error() & { return error_; }
  const Error& error() const& { return error_; }
  Error&& error() && { return std::move(error_); }
  const Error&& error() const&& { return std::move(error_); }
};

template <class Derived, bool, bool Noexcept>
struct CopyConstructible {
  constexpr CopyConstructible() = default;
  CopyConstructible(const CopyConstructible& that) noexcept(Noexcept) {
    static_cast<Derived*>(this)->assign(static_cast<const Derived&>(that));
  }
  constexpr CopyConstructible(CopyConstructible&&) = default;
  CopyConstructible& operator=(const CopyConstructible&) = default;
  CopyConstructible& operator=(CopyConstructible&&) = default;
};

template <class Derived, bool Noexcept>
struct CopyConstructible<Derived, false, Noexcept> {
  constexpr CopyConstructible() = default;
  CopyConstructible(const CopyConstructible&) = delete;
  constexpr CopyConstructible(CopyConstructible&&) = default;
  CopyConstructible& operator=(const CopyConstructible&) = default;
  CopyConstructible& operator=(CopyConstructible&&) = default;
};

template <class Derived, bool, bool Noexcept>
struct MoveConstructible {
  constexpr MoveConstructible() = default;
  constexpr MoveConstructible(const MoveConstructible&) = default;
  MoveConstructible(MoveConstructible&& that) noexcept(Noexcept) {
    static_cast<Derived*>(this)->assign(std::move(static_cast<Derived&>(that)));
  }
  MoveConstructible& operator=(const MoveConstructible&) = default;
  MoveConstructible& operator=(MoveConstructible&&) = default;
};

template <class Derived, bool Noexcept>
struct MoveConstructible<Derived, false, Noexcept> {
  constexpr MoveConstructible() = default;
  constexpr MoveConstructible(const MoveConstructible&) = default;
  MoveConstructible(MoveConstructible&&) = delete;
  MoveConstructible& operator=(const MoveConstructible&) = default;
  MoveConstructible& operator=(MoveConstructible&&) = default;
};

template <class Derived, bool, bool Noexcept>
struct CopyAssignable {
  constexpr CopyAssignable() = default;
  constexpr CopyAssignable(const CopyAssignable&) = default;
  constexpr CopyAssignable(CopyAssignable&&) = default;
  CopyAssignable& operator=(const CopyAssignable& that) noexcept(Noexcept) {
    static_cast<Derived*>(this)->assign(static_cast<const Derived&>(that));
    return *this;
  }
  CopyAssignable& operator=(CopyAssignable&&) = default;
};

template <class Derived, bool Noexcept>
struct CopyAssignable<Derived, false, Noexcept> {
  constexpr CopyAssignable() = default;
  constexpr CopyAssignable(const CopyAssignable&) = default;
  constexpr CopyAssignable(CopyAssignable&&) = default;
  CopyAssignable& operator=(const CopyAssignable&) = delete;
  CopyAssignable& operator=(CopyAssignable&&) = default;
};

template <class Derived, bool, bool Noexcept>
struct MoveAssignable {
  constexpr MoveAssignable() = default;
  constexpr MoveAssignable(const MoveAssignable&) = default;
  constexpr MoveAssignable(MoveAssignable&&) = default;
  MoveAssignable& operator=(const MoveAssignable&) = default;
  MoveAssignable& operator=(MoveAssignable&& that) noexcept(Noexcept) {
    static_cast<Derived*>(this)->assign(std::move(static_cast<Derived&>(that)));
    return *this;
  }
};

template <class Derived, bool Noexcept>
struct MoveAssignable<Derived, false, Noexcept> {
  constexpr MoveAssignable() = default;
  constexpr MoveAssignable(const MoveAssignable&) = default;
  constexpr MoveAssignable(MoveAssignable&&) = default;
  MoveAssignable& operator=(const MoveAssignable&) = default;
  MoveAssignable& operator=(MoveAssignable&& that) = delete;
};

template <class Value, class Error>
struct ExpectedStorage<Value, Error, StorageType::eUnion>
    : ExpectedUnion<Value, Error>,
      CopyConstructible<
          ExpectedStorage<Value, Error, StorageType::eUnion>,
          StrictAllOf<std::is_copy_constructible, Value, Error>::value,
          StrictAllOf<std::is_nothrow_copy_constructible, Value, Error>::value>,
      MoveConstructible<
          ExpectedStorage<Value, Error, StorageType::eUnion>,
          StrictAllOf<std::is_move_constructible, Value, Error>::value,
          StrictAllOf<std::is_nothrow_move_constructible, Value, Error>::value>,
      CopyAssignable<
          ExpectedStorage<Value, Error, StorageType::eUnion>,
          StrictAllOf<IsCopyable, Value, Error>::value,
          StrictAllOf<IsNothrowCopyable, Value, Error>::value>,
      MoveAssignable<
          ExpectedStorage<Value, Error, StorageType::eUnion>,
          StrictAllOf<IsMovable, Value, Error>::value,
          StrictAllOf<IsNothrowMovable, Value, Error>::value> {
  using value_type = Value;
  using error_type = Error;
  using Base = ExpectedUnion<Value, Error>;
  template <class E = Error, class = decltype(E{})>
  constexpr ExpectedStorage() noexcept(noexcept(E{})) : Base{ErrorTag{}} {}
  ExpectedStorage(const ExpectedStorage&) = default;
  ExpectedStorage(ExpectedStorage&&) = default;
  ExpectedStorage& operator=(const ExpectedStorage&) = default;
  ExpectedStorage& operator=(ExpectedStorage&&) = default;
  using ExpectedUnion<Value, Error>::ExpectedUnion;
  ~ExpectedStorage() { clear(); }
  void clear() noexcept {
    switch (this->which_) {
      case Which::eValue:
        this->value().~Value();
        break;
      case Which::eError:
        this->error().~Error();
        break;
      case Which::eEmpty:
      default:
        break;
    }
    this->which_ = Which::eEmpty;
  }
  bool uninitializedByException() const noexcept {
    return this->which_ == Which::eEmpty;
  }
  template <class... Vs>
  void assignValue(Vs&&... vs) {
    auto& val = this->value();
    if (this->which_ == Which::eValue) {
      expected_detail::doEmplaceAssign(0, val, static_cast<Vs&&>(vs)...);
    } else {
      this->clear();
      auto addr =
          const_cast<void*>(static_cast<void const*>(std::addressof(val)));
      ::new (addr) Value(static_cast<Vs&&>(vs)...);
      this->which_ = Which::eValue;
    }
  }
  template <class... Es>
  void assignError(Es&&... es) {
    if (this->which_ == Which::eError) {
      expected_detail::doEmplaceAssign(
          0, this->error(), static_cast<Es&&>(es)...);
    } else {
      this->clear();
      ::new ((void*)std::addressof(this->error()))
          Error(static_cast<Es&&>(es)...);
      this->which_ = Which::eError;
    }
  }
  bool isSelfAssign(const ExpectedStorage* that) const { return this == that; }
  constexpr bool isSelfAssign(const void*) const { return false; }
  template <class Other>
  void assign(Other&& that) {
    if (isSelfAssign(&that)) {
      return;
    }
    FOLLY_PUSH_WARNING
    FOLLY_CLANG_DISABLE_WARNING("-Wcovered-switch-default")
    switch (that.which_) {
      case Which::eValue:
        this->assignValue(static_cast<Other&&>(that).value());
        break;
      case Which::eError:
        this->assignError(static_cast<Other&&>(that).error());
        break;
      case Which::eEmpty:
      default:
        this->clear();
        break;
    }
    FOLLY_POP_WARNING
  }
};

// For small (pointer-sized) trivial types, a struct is faster than a union.
template <class Value, class Error>
struct ExpectedStorage<Value, Error, StorageType::ePODStruct> {
  using value_type = Value;
  using error_type = Error;
  Which which_;
  Error error_;
  Value value_;

  constexpr ExpectedStorage() noexcept
      : which_(Which::eError), error_{}, value_{} {}
  explicit constexpr ExpectedStorage(EmptyTag) noexcept
      : which_(Which::eEmpty), error_{}, value_{} {}
  template <class... Vs>
  explicit constexpr ExpectedStorage(ValueTag, Vs&&... vs) noexcept(
      noexcept(Value(static_cast<Vs&&>(vs)...)))
      : which_(Which::eValue), error_{}, value_(static_cast<Vs&&>(vs)...) {}
  template <class... Es>
  explicit constexpr ExpectedStorage(ErrorTag, Es&&... es) noexcept(
      noexcept(Error(static_cast<Es&&>(es)...)))
      : which_(Which::eError), error_(static_cast<Es&&>(es)...), value_{} {}
  void clear() noexcept {}
  constexpr static bool uninitializedByException() noexcept { return false; }
  template <class... Vs>
  void assignValue(Vs&&... vs) {
    expected_detail::doEmplaceAssign(0, value_, static_cast<Vs&&>(vs)...);
    which_ = Which::eValue;
  }
  template <class... Es>
  void assignError(Es&&... es) {
    expected_detail::doEmplaceAssign(0, error_, static_cast<Es&&>(es)...);
    which_ = Which::eError;
  }
  template <class Other>
  void assign(Other&& that) {
    switch (that.which_) {
      case Which::eValue:
        this->assignValue(static_cast<Other&&>(that).value());
        break;
      case Which::eError:
        this->assignError(static_cast<Other&&>(that).error());
        break;
      case Which::eEmpty:
      default:
        this->clear();
        break;
    }
  }
  Value& value() & { return value_; }
  const Value& value() const& { return value_; }
  Value&& value() && { return std::move(value_); }
  const Value&& value() const&& { return std::move(value_); }
  Error& error() & { return error_; }
  const Error& error() const& { return error_; }
  Error&& error() && { return std::move(error_); }
  const Error&& error() const&& { return std::move(error_); }
};

namespace expected_detail_ExpectedHelper {
// Tricky hack so that Expected::then can handle lambdas that return void
template <class T>
inline T&& operator,(T&& t, Unit) noexcept {
  return static_cast<T&&>(t);
}

struct ExpectedHelper {
  template <typename V, typename E>
  FOLLY_ERASE static void assume_empty(Expected<V, E> const& x) {
    compiler_may_unsafely_assume(x.which_ == Which::eEmpty);
  }

  template <class Error, class T>
  static constexpr Expected<typename std::decay<T>::type, Error> return_(
      T&& t) {
    return folly::makeExpected<Error>(static_cast<T&&>(t));
  }

  template <
      class Error,
      class T,
      class U FOLLY_REQUIRES_TRAILING(
          expected_detail::IsConvertible<U&&, Error>::value)>
  static constexpr Expected<T, Error> return_(Expected<T, U>&& t) {
    return Expected<T, Error>(static_cast<Expected<T, U>&&>(t));
  }

  template <class This>
  static typename std::decay<This>::type then_(This&& ex) {
    return static_cast<This&&>(ex);
  }

  FOLLY_PUSH_WARNING
  // Don't warn about not using the overloaded comma operator.
  FOLLY_MSVC_DISABLE_WARNING(4913)
  // On MSVC in optimized builds, the following functions can throw warning 4702
  // Unreachable Code which will block builds which treat warnings as error.
  FOLLY_MSVC_DISABLE_WARNING(4702)
  template <
      class This,
      class Fn,
      class... Fns,
      class E = ExpectedErrorType<This>,
      class T = ExpectedHelper>
  static auto then_(This&& ex, Fn&& fn, Fns&&... fns) -> decltype(T::then_(
      T::template return_<E>(
          (std::declval<Fn>()(std::declval<This>().value()), unit)),
      std::declval<Fns>()...)) {
    if (ex.which_ == expected_detail::Which::eValue) [[likely]] {
      return T::then_(
          T::template return_<E>(
              // Uses the comma operator defined above IFF the lambda
              // returns non-void.
              (static_cast<Fn&&>(fn)(static_cast<This&&>(ex).value()), unit)),
          static_cast<Fns&&>(fns)...);
    }
    return makeUnexpected(static_cast<This&&>(ex).error());
  }

  template <
      class This,
      class Yes,
      class No,
      class Ret = decltype(std::declval<Yes>()(std::declval<This>().value())),
      class Err = decltype(std::declval<No>()(std::declval<This>().error()))
          FOLLY_REQUIRES_TRAILING(!std::is_void<Err>::value)>
  static Ret thenOrThrow_(This&& ex, Yes&& yes, No&& no) {
    if (ex.which_ == expected_detail::Which::eValue) [[likely]] {
      return Ret(static_cast<Yes&&>(yes)(static_cast<This&&>(ex).value()));
    }
    throw_exception(static_cast<No&&>(no)(static_cast<This&&>(ex).error()));
  }

  template <
      class This,
      class Yes,
      class No,
      class Ret = decltype(std::declval<Yes>()(std::declval<This>().value())),
      class Err = decltype(std::declval<No>()(std::declval<This&>().error()))
          FOLLY_REQUIRES_TRAILING(std::is_void<Err>::value)>
  static Ret thenOrThrow_(This&& ex, Yes&& yes, No&& no) {
    if (ex.which_ == expected_detail::Which::eValue) [[likely]] {
      return Ret(static_cast<Yes&&>(yes)(static_cast<This&&>(ex).value()));
    }
    static_cast<No&&>(no)(ex.error());
    throw_exception<BadExpectedAccess<ExpectedErrorType<This>>>(
        static_cast<This&&>(ex).error());
  }

  /**
   * Note: the condition for specialization is easy to miss here - this is for
   * where Err fails is_void, AND the else chain is not void returning. Invoked
   * when orElse handles a chain.
   */
  template <
      class This,
      class No,
      class... AndFns,
      class E = ExpectedErrorType<This>,
      class V = ExpectedValueType<This>,
      class T = ExpectedHelper,
      class RetValue =
          decltype(T::then_(
                       T::template return_<E>(
                           (std::declval<No>()(std::declval<This>().error()))),
                       std::declval<AndFns>()...)
                       .value()),
      typename std::enable_if<!std::is_same<
          std::remove_reference<RetValue>,
          std::remove_reference<folly::Unit&&>>::value>::type* = nullptr,
      class Err = decltype(std::declval<No>()(std::declval<This&>().error()))
          FOLLY_REQUIRES_TRAILING(!std::is_void<Err>::value)>
  static auto orElse_(This&& ex, No&& no, AndFns&&... fns) -> Expected<V, E> {
    // Note - this basically decays into then_ once the first type (No) is
    // called for the error.
    if (ex.which_ == expected_detail::Which::eValue) [[likely]] {
      return T::template return_<E>((T::then_(
          T::template return_<E>(
              // Uses the comma operator defined above IFF the lambda
              // returns non-void.
              static_cast<decltype(ex)&&>(ex).value()))));
    }
    return T::then_(
        T::template return_<E>(
            (static_cast<No&&>(no)(static_cast<This&&>(ex).error()))),
        static_cast<AndFns&&>(fns)...);
  }

  /**
   * Note: the condition for specialization is easy to miss here - this is for
   * where Err fails is_void AND the else chain is void returning. Invoked when
   * orElse handles a chain.
   */
  template <
      class This,
      class No,
      class... AndFns,
      class E = ExpectedErrorType<This>,
      class T = ExpectedHelper,
      class RetValue =
          decltype(T::then_(
                       T::template return_<E>(
                           (std::declval<No>()(std::declval<This>().error()))),
                       std::declval<AndFns>()...)
                       .value()),
      typename std::enable_if<std::is_same<
          std::remove_reference<RetValue>,
          std::remove_reference<folly::Unit&&>>::value>::type* = nullptr,
      class Err = decltype(std::declval<No>()(std::declval<This&>().error()))
          FOLLY_REQUIRES_TRAILING(!std::is_void<Err>::value)>
  static auto orElse_(This&& ex, No&& no, AndFns&&... fns)
      -> Expected<folly::Unit, E> {
    // Note - this basically decays into then_ once the first type (No) is
    // called for the error.
    if (std::forward<This>(ex).which_ == expected_detail::Which::eValue) {
      // Void returning method on else must either throw, or be replaced with a
      // chain that ends in a valid result.""
      throw_exception<BadExpectedAccess<void>>();
    }
    return T::then_(
        T::template return_<E>(
            (static_cast<No&&>(no)(static_cast<This&&>(ex).error()))),
        static_cast<AndFns&&>(fns)...);
  }

  /**
   * Note: the condition for specialization is easy to miss here - this is for
   * where Err passes is_void. Invoked when orElse handles a void returning
   * func.
   */
  template <
      class This,
      class No,
      class... AndFns,
      class E = ExpectedErrorType<This>,
      class V = ExpectedValueType<This>,
      class T = ExpectedHelper,
      class Err = decltype(std::declval<No>()(std::declval<This&>().error()))
          FOLLY_REQUIRES_TRAILING(std::is_void<Err>::value)>
  static auto orElse_(This&& ex, No&& no, AndFns&&...) -> Expected<V, E> {
    if (ex.which_ == expected_detail::Which::eValue) [[likely]] {
      return return_<E>(static_cast<decltype(ex)&&>(ex).value());
    }
    static_cast<No&&>(no)(static_cast<This&&>(ex).error());
    return makeUnexpected(static_cast<decltype(ex)&&>(ex).error());
  }

  template <
      class This,
      class OnError,
      class Err =
          decltype(std::declval<OnError>()(std::declval<This>().error()))
              FOLLY_REQUIRES_TRAILING(std::is_void<Err>::value)>
  static This onError_(This&& ex, OnError&& onError) {
    if (UNLIKELY(ex.which_ == expected_detail::Which::eError)) {
      static_cast<OnError&&>(onError)(static_cast<This&&>(ex).error());
    }
    return ex;
  }

  FOLLY_POP_WARNING
};
} // namespace expected_detail_ExpectedHelper

struct UnexpectedTag {};

} // namespace expected_detail

using unexpected_t =
    expected_detail::UnexpectedTag (&)(expected_detail::UnexpectedTag);

inline expected_detail::UnexpectedTag unexpected(
    expected_detail::UnexpectedTag = {}) {
  return {};
}

namespace expected_detail {
// empty
} // namespace expected_detail

/**
 * Expected - For holding a value or an error. Useful as an alternative to
 * exceptions, for APIs where throwing on failure would be too expensive.
 *
 * Expected<Value, Error> is a variant over the types Value and Error.
 *
 * Expected does not offer support for references. Use
 * Expected<std::reference_wrapper<T>, Error> if your API needs to return a
 * reference or an error.
 *
 * Expected offers a continuation-based interface to reduce the boilerplate
 * of checking error codes. The Expected::then member function takes a lambda
 * that is to execute should the Expected object contain a value. The return
 * value of the lambda is wrapped in an Expected and returned. If the lambda is
 * not executed because the Expected contains an error, the error is returned
 * immediately in a new Expected object.
 *
 * Expected<int, Error> funcTheFirst();
 * Expected<std::string, Error> funcTheSecond() {
 *   return funcTheFirst().then([](int i) { return std::to_string(i); });
 * }
 *
 * The above line of code could more verbosely written as:
 *
 * Expected<std::string, Error> funcTheSecond() {
 *   if (auto ex = funcTheFirst()) {
 *     return std::to_string(*ex);
 *   }
 *   return makeUnexpected(ex.error());
 * }
 *
 * Continuations can chain, like:
 *
 * Expected<D, Error> maybeD = someFunc()
 *     .then([](A a){return B(a);})
 *     .then([](B b){return C(b);})
 *     .then([](C c){return D(c);});
 *
 * To avoid the redundant error checking that would happen if a call at the
 * front of the chain returns an error, these call chains can be collapsed into
 * a single call to .then:
 *
 * Expected<D, Error> maybeD = someFunc()
 *     .then([](A a){return B(a);},
 *           [](B b){return C(b);},
 *           [](C c){return D(c);});
 *
 * The result of .then() is wrapped into Expected< ~, Error > if it isn't
 * of that form already. Consider the following code:
 *
 * extern Expected<std::string, Error> readLineFromIO();
 * extern Expected<int, Error> parseInt(std::string);
 * extern int increment(int);
 *
 * Expected<int, Error> x = readLineFromIO().then(parseInt).then(increment);
 *
 * From the code above, we see that .then() works both with functions that
 * return an Expected< ~, Error > (like parseInt) and with ones that return
 * a plain value (like increment). In the case of parseInt, .then() returns
 * the result of parseInt as-is. In the case of increment, it wraps the int
 * that increment returns into an Expected< int, Error >.
 *
 * Sometimes when using a continuation you would prefer an exception to be
 * thrown for a value-less Expected. For that you can use .thenOrThrow, as
 * follows:
 *
 * B b = someFunc()
 *     .thenOrThrow([](A a){return B(a);});
 *
 * The above call to thenOrThrow will invoke the lambda if the Expected returned
 * by someFunc() contains a value. Otherwise, it will throw an exception of type
 * Unexpected<Error>::BadExpectedAccess. If you prefer it throw an exception of
 * a different type, you can pass a second lambda to thenOrThrow:
 *
 * B b = someFunc()
 *     .thenOrThrow([](A a){return B(a);},
 *                  [](Error e) {throw MyException(e);});
 *
 * Like C++17's std::variant, Expected offers the almost-never-empty guarantee;
 * that is, an Expected<Value, Error> almost always contains either a Value or
 * and Error. Partially-formed Expected objects occur when an assignment to
 * an Expected object that would change the type of the contained object (Value-
 * to-Error or vice versa) throws. Trying to access either the contained value
 * or error object causes Expected to throw folly::BadExpectedAccess.
 *
 * Expected models OptionalPointer, so calling 'get_pointer(ex)' will return a
 * pointer to nullptr if the 'ex' is in the error state, and a pointer to the
 * value otherwise:
 *
 *  Expected<int, Error> maybeInt = ...;
 *  if (int* v = get_pointer(maybeInt)) {
 *    cout << *v << endl;
 *  }
 */
template <class Value, class Error>
class Expected final : expected_detail::ExpectedStorage<Value, Error> {
  template <class, class>
  friend class Expected;
  template <class, class, expected_detail::StorageType>
  friend struct expected_detail::ExpectedStorage;
  friend struct expected_detail::ExpectedHelper;
  using Base = expected_detail::ExpectedStorage<Value, Error>;
  Base& base() & { return *this; }
  const Base& base() const& { return *this; }
  Base&& base() && { return std::move(*this); }

  struct MakeBadExpectedAccess {
    template <class E>
    auto operator()(E&& e) {
      return BadExpectedAccess<Error>(static_cast<E&&>(e));
    }
  };

 public:
  using value_type = Value;
  using error_type = Error;

  template <class U>
  using rebind = Expected<U, Error>;

  using promise_type = expected_detail::Promise<Value, Error>;

  static_assert(
      !std::is_reference<Value>::value,
      "Expected may not be used with reference types");
  static_assert(
      !std::is_abstract<Value>::value,
      "Expected may not be used with abstract types");

  /*
   * Constructors
   */
  template <class B = Base, class = decltype(B{})>
  Expected() noexcept(noexcept(B{})) : Base{} {}
  Expected(const Expected& that) = default;
  Expected(Expected&& that) = default;

  template <
      class V,
      class E FOLLY_REQUIRES_TRAILING(
          !std::is_same<Expected<V, E>, Expected>::value &&
          std::is_constructible<Value, V&&>::value &&
          std::is_constructible<Error, E&&>::value)>
  Expected(Expected<V, E> that) : Base{expected_detail::EmptyTag{}} {
    this->assign(std::move(that));
  }

  FOLLY_REQUIRES(std::is_copy_constructible<Value>::value)
  constexpr /* implicit */ Expected(const Value& val) noexcept(
      noexcept(Value(val)))
      : Base{expected_detail::ValueTag{}, val} {}

  FOLLY_REQUIRES(std::is_move_constructible<Value>::value)
  constexpr /* implicit */ Expected(Value&& val) noexcept(
      noexcept(Value(std::move(val))))
      : Base{expected_detail::ValueTag{}, std::move(val)} {}

  template <class T FOLLY_REQUIRES_TRAILING(
      std::is_convertible<T, Value>::value &&
      !std::is_convertible<T, Error>::value)>
  constexpr /* implicit */ Expected(T&& val) noexcept(
      noexcept(Value(static_cast<T&&>(val))))
      : Base{expected_detail::ValueTag{}, static_cast<T&&>(val)} {}

  template <class... Ts FOLLY_REQUIRES_TRAILING(
      std::is_constructible<Value, Ts&&...>::value)>
  explicit constexpr Expected(std::in_place_t, Ts&&... ts) noexcept(
      noexcept(Value(std::declval<Ts>()...)))
      : Base{expected_detail::ValueTag{}, static_cast<Ts&&>(ts)...} {}

  template <
      class U,
      class... Ts FOLLY_REQUIRES_TRAILING(
          std::is_constructible<Value, std::initializer_list<U>&, Ts&&...>::
              value)>
  explicit constexpr Expected(
      std::in_place_t,
      std::initializer_list<U> il,
      Ts&&... ts) noexcept(noexcept(Value(std::declval<Ts>()...)))
      : Base{expected_detail::ValueTag{}, il, static_cast<Ts&&>(ts)...} {}

  // If overload resolution selects one of these deleted functions, that
  // means you need to use makeUnexpected
  /* implicit */ Expected(const Error&) = delete;
  /* implicit */ Expected(Error&&) = delete;

  FOLLY_REQUIRES(std::is_copy_constructible<Error>::value)
  constexpr Expected(unexpected_t, const Error& err) noexcept(
      noexcept(Error(err)))
      : Base{expected_detail::ErrorTag{}, err} {}

  FOLLY_REQUIRES(std::is_move_constructible<Error>::value)
  constexpr Expected(unexpected_t, Error&& err) noexcept(
      noexcept(Error(std::move(err))))
      : Base{expected_detail::ErrorTag{}, std::move(err)} {}

  FOLLY_REQUIRES(std::is_copy_constructible<Error>::value)
  constexpr /* implicit */ Expected(const Unexpected<Error>& err) noexcept(
      noexcept(Error(err.error())))
      : Base{expected_detail::ErrorTag{}, err.error()} {}

  FOLLY_REQUIRES(std::is_move_constructible<Error>::value)
  constexpr /* implicit */ Expected(Unexpected<Error>&& err) noexcept(
      noexcept(Error(std::move(err.error()))))
      : Base{expected_detail::ErrorTag{}, std::move(err.error())} {}

  template <class OtherError FOLLY_REQUIRES_TRAILING(
      std::is_convertible<const OtherError&, Error>::value)>
  constexpr /* implicit */ Expected(const Unexpected<OtherError>& err) noexcept(
      noexcept(Error(err.error())))
      : Base{expected_detail::ErrorTag{}, Error(err.error())} {}

  template <class OtherError FOLLY_REQUIRES_TRAILING(
      std::is_convertible<OtherError&&, Error>::value)>
  constexpr /* implicit  */ Expected(Unexpected<OtherError>&& err) noexcept(
      noexcept(Error(std::move(err.error()))))
      : Base{expected_detail::ErrorTag{}, Error(std::move(err.error()))} {}

  /*
   * Assignment operators
   */
  Expected& operator=(const Expected& that) = default;
  Expected& operator=(Expected&& that) = default;

  template <
      class V,
      class E FOLLY_REQUIRES_TRAILING(
          !std::is_same<Expected<V, E>, Expected>::value &&
          expected_detail::IsConvertible<V&&, Value>::value &&
          expected_detail::IsConvertible<E&&, Error>::value)>
  Expected& operator=(Expected<V, E> that) {
    this->assign(std::move(that));
    return *this;
  }

  FOLLY_REQUIRES(expected_detail::IsCopyable<Value>::value)
  Expected& operator=(const Value& val) noexcept(
      expected_detail::IsNothrowCopyable<Value>::value) {
    this->assignValue(val);
    return *this;
  }

  FOLLY_REQUIRES(expected_detail::IsMovable<Value>::value)
  Expected& operator=(Value&& val) noexcept(
      expected_detail::IsNothrowMovable<Value>::value) {
    this->assignValue(std::move(val));
    return *this;
  }

  template <class T FOLLY_REQUIRES_TRAILING(
      std::is_convertible<T, Value>::value &&
      !std::is_convertible<T, Error>::value)>
  Expected& operator=(T&& val) {
    this->assignValue(static_cast<T&&>(val));
    return *this;
  }

  FOLLY_REQUIRES(expected_detail::IsCopyable<Error>::value)
  Expected& operator=(const Unexpected<Error>& err) noexcept(
      expected_detail::IsNothrowCopyable<Error>::value) {
    this->assignError(err.error());
    return *this;
  }

  FOLLY_REQUIRES(expected_detail::IsMovable<Error>::value)
  Expected& operator=(Unexpected<Error>&& err) noexcept(
      expected_detail::IsNothrowMovable<Error>::value) {
    this->assignError(std::move(err.error()));
    return *this;
  }

  template <class... Ts FOLLY_REQUIRES_TRAILING(
      std::is_constructible<Value, Ts&&...>::value)>
  void emplace(Ts&&... ts) {
    this->assignValue(static_cast<Ts&&>(ts)...);
  }

  /**
   * swap
   */
  void swap(Expected& that) noexcept(
      std::is_nothrow_swappable_v<Value> &&
      std::is_nothrow_swappable_v<Error>) {
    if (this->uninitializedByException() || that.uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    using std::swap;
    if (*this) {
      if (that) {
        swap(this->value_, that.value_);
      } else {
        Error e(std::move(that.error_));
        that.assignValue(std::move(this->value_));
        this->assignError(std::move(e));
      }
    } else {
      if (!that) {
        swap(this->error_, that.error_);
      } else {
        Error e(std::move(this->error_));
        this->assignValue(std::move(that.value_));
        that.assignError(std::move(e));
      }
    }
  }

  // If overload resolution selects one of these deleted functions, that
  // means you need to use makeUnexpected
  /* implicit */ Expected& operator=(const Error&) = delete;
  /* implicit */ Expected& operator=(Error&&) = delete;

  /**
   * Relational Operators
   */
  template <class Val, class Err>
  friend typename std::enable_if<IsEqualityComparable<Val>::value, bool>::type
  operator==(const Expected<Val, Err>& lhs, const Expected<Val, Err>& rhs);
  template <class Val, class Err>
  friend typename std::enable_if<IsLessThanComparable<Val>::value, bool>::type
  operator<(const Expected<Val, Err>& lhs, const Expected<Val, Err>& rhs);

  /*
   * Accessors
   */
  constexpr bool hasValue() const noexcept {
    return FOLLY_LIKELY(expected_detail::Which::eValue == this->which_);
  }

  constexpr bool hasError() const noexcept {
    return FOLLY_UNLIKELY(expected_detail::Which::eError == this->which_);
  }

  using Base::uninitializedByException;

  const Value& value() const& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    requireValue();
    return this->Base::value();
  }

  Value& value() & [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    requireValue();
    return this->Base::value();
  }

  const Value&& value() const&& {
    requireValueMove();
    return std::move(this->Base::value());
  }

  Value&& value() && {
    requireValueMove();
    return std::move(this->Base::value());
  }

  const Error& error() const& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    requireError();
    return this->Base::error();
  }

  Error& error() & [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    requireError();
    return this->Base::error();
  }

  const Error&& error() const&& {
    requireError();
    return std::move(this->Base::error());
  }

  Error&& error() && {
    requireError();
    return std::move(this->Base::error());
  }

  // Return a copy of the value if set, or a given default if not.
  template <class U>
  Value value_or(U&& dflt) const& {
    if (this->which_ == expected_detail::Which::eValue) [[likely]] {
      return this->value_;
    }
    return static_cast<U&&>(dflt);
  }

  template <class U>
  Value value_or(U&& dflt) && {
    if (this->which_ == expected_detail::Which::eValue) [[likely]] {
      return std::move(this->value_);
    }
    return static_cast<U&&>(dflt);
  }

  explicit constexpr operator bool() const noexcept { return hasValue(); }

  const Value& operator*() const& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return this->value();
  }

  Value& operator*() & [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return this->value();
  }

  Value&& operator*() && { return std::move(std::move(*this).value()); }

  const Value* operator->() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::addressof(this->value());
  }

  Value* operator->() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::addressof(this->value());
  }

  const Value* get_pointer() const& noexcept {
    return hasValue() ? std::addressof(this->value_) : nullptr;
  }

  Value* get_pointer() & noexcept {
    return hasValue() ? std::addressof(this->value_) : nullptr;
  }

  Value* get_pointer() && = delete;

  /**
   * then
   */
  template <class... Fns FOLLY_REQUIRES_TRAILING(sizeof...(Fns) >= 1)>
  auto
  then(Fns&&... fns) const& -> decltype(expected_detail::ExpectedHelper::then_(
      std::declval<const Base&>(), std::declval<Fns>()...)) {
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    return expected_detail::ExpectedHelper::then_(
        base(), static_cast<Fns&&>(fns)...);
  }

  template <class... Fns FOLLY_REQUIRES_TRAILING(sizeof...(Fns) >= 1)>
  auto then(Fns&&... fns) & -> decltype(expected_detail::ExpectedHelper::then_(
      std::declval<Base&>(), std::declval<Fns>()...)) {
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    return expected_detail::ExpectedHelper::then_(
        base(), static_cast<Fns&&>(fns)...);
  }

  template <class... Fns FOLLY_REQUIRES_TRAILING(sizeof...(Fns) >= 1)>
  auto then(Fns&&... fns) && -> decltype(expected_detail::ExpectedHelper::then_(
      std::declval<Base&&>(), std::declval<Fns>()...)) {
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    return expected_detail::ExpectedHelper::then_(
        std::move(base()), static_cast<Fns&&>(fns)...);
  }

  /**
   * orElse - returns if it has a value, otherwise it calls a function with the
   * error type
   */
  template <class... Fns FOLLY_REQUIRES_TRAILING(sizeof...(Fns) >= 1)>
  auto orElse(Fns&&... fns)
      const& -> decltype(expected_detail::ExpectedHelper::orElse_(
          std::declval<const Base&>(), std::declval<Fns>()...)) {
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    return expected_detail::ExpectedHelper::orElse_(
        base(), static_cast<Fns&&>(fns)...);
  }

  template <class... Fns FOLLY_REQUIRES_TRAILING(sizeof...(Fns) >= 1)>
  auto
  orElse(Fns&&... fns) & -> decltype(expected_detail::ExpectedHelper::orElse_(
      std::declval<Base&>(), std::declval<Fns>()...)) {
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    return expected_detail::ExpectedHelper::orElse_(
        base(), static_cast<Fns&&>(fns)...);
  }

  template <class... Fns FOLLY_REQUIRES_TRAILING(sizeof...(Fns) >= 1)>
  auto
  orElse(Fns&&... fns) && -> decltype(expected_detail::ExpectedHelper::orElse_(
      std::declval<Base&&>(), std::declval<Fns>()...)) {
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    return expected_detail::ExpectedHelper::orElse_(
        std::move(base()), static_cast<Fns&&>(fns)...);
  }

  /**
   * thenOrThrow
   */
  template <class Yes, class No = MakeBadExpectedAccess>
  auto thenOrThrow(Yes&& yes, No&& no = No{})
      const& -> decltype(std::declval<Yes>()(std::declval<const Value&>())) {
    using Ret = decltype(std::declval<Yes>()(std::declval<const Value&>()));
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    return Ret(
        expected_detail::ExpectedHelper::thenOrThrow_(
            base(), static_cast<Yes&&>(yes), static_cast<No&&>(no)));
  }

  template <class Yes, class No = MakeBadExpectedAccess>
  auto thenOrThrow(Yes&& yes, No&& no = No{}) & -> decltype(std::declval<Yes>()(
      std::declval<Value&>())) {
    using Ret = decltype(std::declval<Yes>()(std::declval<Value&>()));
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    return Ret(
        expected_detail::ExpectedHelper::thenOrThrow_(
            base(), static_cast<Yes&&>(yes), static_cast<No&&>(no)));
  }

  template <class Yes, class No = MakeBadExpectedAccess>
  auto
  thenOrThrow(Yes&& yes, No&& no = No{}) && -> decltype(std::declval<Yes>()(
      std::declval<Value&&>())) {
    using Ret = decltype(std::declval<Yes>()(std::declval<Value&&>()));
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    return Ret(
        expected_detail::ExpectedHelper::thenOrThrow_(
            std::move(base()), static_cast<Yes&&>(yes), static_cast<No&&>(no)));
  }

  /**
   * onError
   */
  template <class OnError>
  auto onError(OnError&& onError) const& {
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }

    return expected_detail::ExpectedHelper::onError_(
        *this, static_cast<OnError&&>(onError));
  }

  template <class OnError>
  auto onError(OnError&& onError) & {
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    return expected_detail::ExpectedHelper::onError_(
        *this, static_cast<OnError&&>(onError));
  }

  template <class OnError>
  auto onError(OnError&& onError) && {
    if (this->uninitializedByException()) {
      throw_exception<BadExpectedAccess<void>>();
    }
    return expected_detail::ExpectedHelper::onError_(
        std::move(*this), static_cast<OnError&&>(onError));
  }

 private:
  friend struct expected_detail::PromiseReturn<Value, Error>;
  using EmptyTag = expected_detail::EmptyTag;

  explicit Expected(EmptyTag tag) noexcept : Base{tag} {}
  // for when coroutine promise return-object conversion is eager
  Expected(EmptyTag tag, Expected*& pointer) noexcept : Base{tag} {
    pointer = this;
  }

  void requireValue() const {
    if (!hasValue()) [[unlikely]] {
      if (hasError()) [[likely]] {
        throw_exception<BadExpectedAccess<Error>>(this->error_);
      }
      throw_exception<BadExpectedAccess<void>>();
    }
  }

  template <typename Self>
  static void requireValueMove(Self& self) {
    if (!self.hasValue()) [[unlikely]] {
      if (self.hasError()) [[likely]] {
        throw_exception<BadExpectedAccess<Error>>(std::move(self.error_));
      }
      throw_exception<BadExpectedAccess<void>>();
    }
  }

  void requireValueMove() { return requireValueMove(*this); }
  void requireValueMove() const { return requireValueMove(*this); }

  void requireError() const {
    if (!hasError()) [[unlikely]] {
      throw_exception<BadExpectedAccess<void>>();
    }
  }

  expected_detail::Which which() const noexcept { return this->which_; }
};

template <class Value, class Error>
inline typename std::enable_if<IsEqualityComparable<Value>::value, bool>::type
operator==(
    const Expected<Value, Error>& lhs, const Expected<Value, Error>& rhs) {
  if (lhs.uninitializedByException()) [[unlikely]] {
    throw_exception<BadExpectedAccess<void>>();
  }
  if (lhs.which_ != rhs.which_) [[unlikely]] {
    return false;
  }
  if (lhs.hasError()) [[unlikely]] {
    return true; // All error states are considered equal
  }
  return lhs.value_ == rhs.value_;
}

template <class Value, class Error>
inline typename std::enable_if<IsLessThanComparable<Value>::value, bool>::type
operator<(
    const Expected<Value, Error>& lhs, const Expected<Value, Error>& rhs) {
  if (lhs.uninitializedByException() || rhs.uninitializedByException())
      [[unlikely]] {
    throw_exception<BadExpectedAccess<void>>();
  }
  if (lhs.hasError()) [[unlikely]] {
    return !rhs.hasError();
  }
  if (rhs.hasError()) [[unlikely]] {
    return false;
  }
  return lhs.value_ < rhs.value_;
}

template <
    class Value,
    class Error FOLLY_REQUIRES_TRAILING(IsLessThanComparable<Value>::value)>
inline bool operator<=(
    const Expected<Value, Error>& lhs, const Expected<Value, Error>& rhs) {
  return !(rhs < lhs);
}

template <
    class Value,
    class Error FOLLY_REQUIRES_TRAILING(IsLessThanComparable<Value>::value)>
inline bool operator>(
    const Expected<Value, Error>& lhs, const Expected<Value, Error>& rhs) {
  return rhs < lhs;
}

template <
    class Value,
    class Error FOLLY_REQUIRES_TRAILING(IsLessThanComparable<Value>::value)>
inline bool operator>=(
    const Expected<Value, Error>& lhs, const Expected<Value, Error>& rhs) {
  return !(lhs < rhs);
}

/**
 * swap Expected values
 */
template <class Value, class Error>
void swap(Expected<Value, Error>& lhs, Expected<Value, Error>& rhs) noexcept(
    std::is_nothrow_swappable_v<Value> && std::is_nothrow_swappable_v<Error>) {
  lhs.swap(rhs);
}

template <class Value, class Error>
const Value* get_pointer(const Expected<Value, Error>& ex) noexcept {
  return ex.get_pointer();
}

template <class Value, class Error>
Value* get_pointer(Expected<Value, Error>& ex) noexcept {
  return ex.get_pointer();
}

/**
 * For constructing an Expected object from a value, with the specified
 * Error type. Usage is as follows:
 *
 * enum MyErrorCode { BAD_ERROR, WORSE_ERROR };
 * Expected<int, MyErrorCode> myAPI() {
 *   int i = // ...;
 *   return i ? makeExpected<MyErrorCode>(i) : makeUnexpected(BAD_ERROR);
 * }
 */
template <class Error, class Value>
[[nodiscard]] constexpr Expected<typename std::decay<Value>::type, Error>
makeExpected(Value&& val) {
  return Expected<typename std::decay<Value>::type, Error>{
      std::in_place, static_cast<Value&&>(val)};
}

// Suppress comparability of Expected<T> with T, despite implicit conversion.
template <class Value, class Error>
bool operator==(const Expected<Value, Error>&, const Value& other) = delete;
template <class Value, class Error>
bool operator<(const Expected<Value, Error>&, const Value& other) = delete;
template <class Value, class Error>
bool operator<=(const Expected<Value, Error>&, const Value& other) = delete;
template <class Value, class Error>
bool operator>=(const Expected<Value, Error>&, const Value& other) = delete;
template <class Value, class Error>
bool operator>(const Expected<Value, Error>&, const Value& other) = delete;
template <class Value, class Error>
bool operator<(const Value& other, const Expected<Value, Error>&) = delete;
template <class Value, class Error>
bool operator<=(const Value& other, const Expected<Value, Error>&) = delete;
template <class Value, class Error>
bool operator>=(const Value& other, const Expected<Value, Error>&) = delete;
template <class Value, class Error>
bool operator>(const Value& other, const Expected<Value, Error>&) = delete;

} // namespace folly

#undef FOLLY_REQUIRES
#undef FOLLY_REQUIRES_TRAILING

// Enable the use of folly::Expected with `co_await`
// Inspired by https://github.com/toby-allsopp/coroutine_monad
//
// NOTE: Please port applicable improvements to `folly/result/coro.h`.
#if FOLLY_HAS_COROUTINES
#include <folly/coro/Coroutine.h>

namespace folly {
namespace expected_detail {
template <typename Value, typename Error>
struct PromiseBase;

template <typename Value, typename Error>
struct PromiseReturn {
  Expected<Value, Error> storage_{EmptyTag{}};
  Expected<Value, Error>*& pointer_;

  /* implicit */ PromiseReturn(PromiseBase<Value, Error>& p) noexcept
      : pointer_{p.value_} {
    pointer_ = &storage_;
  }
  PromiseReturn(PromiseReturn const&) = delete;
  // letting dtor be trivial makes the coroutine crash
  // TODO: fix clang/llvm codegen
  ~PromiseReturn() {}
  /* implicit */ operator Expected<Value, Error>() {
    // handle both deferred and eager return-object conversion behaviors
    // see docs for detect_promise_return_object_eager_conversion
    if (folly::coro::detect_promise_return_object_eager_conversion()) {
      assert(storage_.which_ == expected_detail::Which::eEmpty);
      return Expected<Value, Error>{EmptyTag{}, pointer_}; // eager
    } else {
      assert(storage_.which_ != expected_detail::Which::eEmpty);
      return std::move(storage_); // deferred
    }
  }
};

template <typename Value, typename Error>
struct PromiseBase {
  Expected<Value, Error>* value_ = nullptr;

  PromiseBase() = default;
  PromiseBase(PromiseBase const&) = delete;
  void operator=(PromiseBase const&) = delete;

  coro::suspend_never initial_suspend() const noexcept { return {}; }
  coro::suspend_never final_suspend() const noexcept { return {}; }
  [[noreturn]] void unhandled_exception() {
    // Technically, throwing from unhandled_exception is underspecified:
    // https://github.com/GorNishanov/CoroutineWording/issues/17
    rethrow_current_exception();
  }

  PromiseReturn<Value, Error> get_return_object() noexcept { return *this; }
};

template <typename Value>
inline constexpr bool ReturnsVoid =
    std::is_trivial_v<Value> && std::is_empty_v<Value>;

template <typename Value, typename Error>
struct PromiseReturnsValue : public PromiseBase<Value, Error> {
  template <typename U = Value>
  void return_value(U&& u) {
    auto& v = *this->value_;
    ExpectedHelper::assume_empty(v);
    v = static_cast<U&&>(u);
  }
};

template <typename Value, typename Error>
struct PromiseReturnsVoid : public PromiseBase<Value, Error> {
  // When the coroutine uses `return;` you can fail via `co_await err`.
  void return_void() { this->value_->emplace(Value{}); }
};

template <typename Value, typename Error>
struct Promise //
    : conditional_t<
          ReturnsVoid<Value>,
          PromiseReturnsVoid<Value, Error>,
          PromiseReturnsValue<Value, Error>> {};

template <typename Error>
struct UnexpectedAwaitable {
  Unexpected<Error> o_;

  explicit UnexpectedAwaitable(Unexpected<Error> o) : o_(std::move(o)) {}

  constexpr std::false_type await_ready() const noexcept { return {}; }
  void await_resume() { compiler_may_unsafely_assume_unreachable(); }

  template <typename U>
  FOLLY_ALWAYS_INLINE void
  await_suspend(coro::coroutine_handle<Promise<U, Error>> h) noexcept(
      IsNothrowMovable<Error>::value) {
    auto& v = *h.promise().value_;
    ExpectedHelper::assume_empty(v);
    v = std::move(o_);
    h.destroy();
  }
};

template <typename Value, typename Error>
struct ExpectedAwaitable {
  Expected<Value, Error> o_;

  explicit ExpectedAwaitable(Expected<Value, Error> o) : o_(std::move(o)) {}

  bool await_ready() const noexcept { return o_.hasValue(); }
  Value await_resume() { return std::move(o_.value()); }

  // Explicitly only allow suspension into a Promise
  template <typename U>
  FOLLY_ALWAYS_INLINE void
  await_suspend(coro::coroutine_handle<Promise<U, Error>> h) noexcept(
      IsNothrowMovable<Error>::value) {
    auto& v = *h.promise().value_;
    ExpectedHelper::assume_empty(v);
    v = makeUnexpected(std::move(o_.error()));
    // Abort the rest of the coroutine. resume() is not going to be called
    h.destroy();
  }
};

} // namespace expected_detail

template <typename Error>
expected_detail::UnexpectedAwaitable<Error>
/* implicit */ operator co_await(Unexpected<Error> o) {
  return expected_detail::UnexpectedAwaitable<Error>{std::move(o)};
}

template <typename Value, typename Error>
expected_detail::ExpectedAwaitable<Value, Error>
/* implicit */ operator co_await(Expected<Value, Error> o) {
  return expected_detail::ExpectedAwaitable<Value, Error>{std::move(o)};
}

} // namespace folly
#endif // FOLLY_HAS_COROUTINES
