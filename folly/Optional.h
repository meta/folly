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

//
// Docs: https://fburl.com/fbcref_optional
//

#pragma once

/*
 * Optional - For conditional initialization of values, like boost::optional,
 * but with support for move semantics and emplacement.  Reference type support
 * has not been included due to limited use cases and potential confusion with
 * semantics of assignment: Assigning to an optional reference could quite
 * reasonably copy its value or redirect the reference.
 *
 * Optional can be useful when a variable might or might not be needed:
 *
 *  Optional<Logger> maybeLogger = ...;
 *  if (maybeLogger) {
 *    maybeLogger->log("hello");
 *  }
 *
 * Optional enables a 'null' value for types which do not otherwise have
 * nullability, especially useful for parameter passing:
 *
 * void testIterator(const unique_ptr<Iterator>& it,
 *                   initializer_list<int> idsExpected,
 *                   Optional<initializer_list<int>> ranksExpected = none) {
 *   for (int i = 0; it->next(); ++i) {
 *     EXPECT_EQ(it->doc().id(), idsExpected[i]);
 *     if (ranksExpected) {
 *       EXPECT_EQ(it->doc().rank(), (*ranksExpected)[i]);
 *     }
 *   }
 * }
 *
 * Optional models OptionalPointee, so calling 'get_pointer(opt)' will return a
 * pointer to nullptr if the 'opt' is empty, and a pointer to the value if it is
 * not:
 *
 *  Optional<int> maybeInt = ...;
 *  if (int* v = get_pointer(maybeInt)) {
 *    cout << *v << endl;
 *  }
 */

#include <cassert>
#include <compare>
#include <concepts>
#include <cstddef>
#include <functional>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <folly/CppAttributes.h>
#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/hash/traits.h>
#include <folly/lang/Exception.h>

namespace folly {

template <class Value>
class Optional;

namespace detail {
struct OptionalEmptyTag {};
template <class Value>
struct OptionalPromise;
template <class Value>
struct OptionalPromiseReturn;

// Storage types for Optional, parameterized on Value.
template <class Value>
struct OptionalStorageTriviallyDestructible {
  union {
    char emptyState;
    Value value;
  };
  bool hasValue;

  constexpr OptionalStorageTriviallyDestructible()
      : emptyState(unsafe_default_initialized), hasValue{false} {}
  void clear() { hasValue = false; }
};

template <class Value>
struct OptionalStorageNonTriviallyDestructible {
  union {
    char emptyState;
    Value value;
  };
  bool hasValue;

  constexpr OptionalStorageNonTriviallyDestructible() : hasValue{false} {}
  ~OptionalStorageNonTriviallyDestructible() { clear(); }
  OptionalStorageNonTriviallyDestructible(
      const OptionalStorageNonTriviallyDestructible&) = delete;
  OptionalStorageNonTriviallyDestructible& operator=(
      const OptionalStorageNonTriviallyDestructible&) = delete;
  OptionalStorageNonTriviallyDestructible(
      OptionalStorageNonTriviallyDestructible&&) = delete;
  OptionalStorageNonTriviallyDestructible& operator=(
      OptionalStorageNonTriviallyDestructible&&) = delete;

  void clear() {
    if (hasValue) {
      hasValue = false;
      FOLLY_PUSH_WARNING
      FOLLY_GCC_DISABLE_WARNING("-Wmaybe-uninitialized")
      value.~Value();
      FOLLY_POP_WARNING
    }
  }
};

// Base class holding the storage member and construct helper.
template <class Value>
struct OptionalStorage {
  using Storage = typename std::conditional<
      std::is_trivially_destructible_v<Value>,
      OptionalStorageTriviallyDestructible<Value>,
      OptionalStorageNonTriviallyDestructible<Value>>::type;

  Storage storage_;

  constexpr OptionalStorage() noexcept = default;

  template <class... Args>
  void construct(Args&&... args) {
    const void* ptr = &storage_.value;
    new (const_cast<void*>(ptr)) Value(std::forward<Args>(args)...);
    storage_.hasValue = true;
  }
};

// Trivial case: when Value is trivially copy constructible AND trivially
// destructible, all special members are implicitly defaulted (trivial).
template <
    class Value,
    bool = std::is_trivially_copy_constructible_v<Value> &&
        std::is_trivially_destructible_v<Value>>
struct OptionalCopyBase : OptionalStorage<Value> {
  using OptionalStorage<Value>::OptionalStorage;
};

// Non-trivial case: user-defined copy constructor.
template <class Value>
struct OptionalCopyBase<Value, false> : OptionalStorage<Value> {
  using OptionalStorage<Value>::OptionalStorage;
  OptionalCopyBase() = default;
  OptionalCopyBase(const OptionalCopyBase& src) noexcept(
      std::is_nothrow_copy_constructible_v<Value>) {
    if (src.storage_.hasValue) {
      this->construct(src.storage_.value);
    }
  }
  OptionalCopyBase(OptionalCopyBase&&) noexcept = default;
  OptionalCopyBase& operator=(const OptionalCopyBase&) = default;
  OptionalCopyBase& operator=(OptionalCopyBase&&) noexcept = default;
  ~OptionalCopyBase() = default;
};

// Trivial case: when Value is trivially copy constructible, trivially copy
// assignable, AND trivially destructible, the copy assignment operator is
// implicitly defaulted (trivial).
template <
    class Value,
    bool = std::is_trivially_copy_constructible_v<Value> &&
        std::is_trivially_copy_assignable_v<Value> &&
        std::is_trivially_destructible_v<Value>>
struct OptionalCopyAssignBase : OptionalCopyBase<Value> {
  using OptionalCopyBase<Value>::OptionalCopyBase;
};

// Non-trivial case: user-defined copy assignment operator.
template <class Value>
struct OptionalCopyAssignBase<Value, false> : OptionalCopyBase<Value> {
  using OptionalCopyBase<Value>::OptionalCopyBase;
  OptionalCopyAssignBase() = default;
  OptionalCopyAssignBase(const OptionalCopyAssignBase&) = default;
  OptionalCopyAssignBase(OptionalCopyAssignBase&&) noexcept = default;
  ~OptionalCopyAssignBase() = default;
  OptionalCopyAssignBase& operator=(OptionalCopyAssignBase&& other) noexcept =
      default;
  OptionalCopyAssignBase& operator=(const OptionalCopyAssignBase& src) noexcept(
      std::is_nothrow_copy_constructible_v<Value> &&
      std::is_nothrow_copy_assignable_v<Value>) {
    if (this->storage_.hasValue && src.storage_.hasValue) {
      this->storage_.value = src.storage_.value;
    } else if (src.storage_.hasValue) {
      this->construct(src.storage_.value);
    } else {
      this->storage_.clear();
    }
    return *this;
  }
};

} // namespace detail

struct None {
  enum class _secret { _token };

  /**
   * No default constructor to support both `op = {}` and `op = none`
   * as syntax for clearing an Optional, just like std::nullopt_t.
   */
  explicit constexpr None(_secret) {}
};
constexpr None none{None::_secret::_token};

class FOLLY_EXPORT OptionalEmptyException : public std::runtime_error {
 public:
  OptionalEmptyException()
      : std::runtime_error("Empty Optional cannot be unwrapped") {}
};

/**
 * Optional is superseded by std::optional. Now that the C++ has a standardized
 * implementation, Optional exists primarily for backward compatibility.
 */
template <class Value>
class Optional : private detail::OptionalCopyAssignBase<Value> {
  using Base = detail::OptionalCopyAssignBase<Value>;

  static constexpr inline bool kIsCopyAssignable =
      std::is_copy_constructible_v<Value> && std::is_copy_assignable_v<Value>;
  static constexpr inline bool kIsMoveAssignable =
      std::is_move_constructible_v<Value> && std::is_move_assignable_v<Value>;
  static constexpr inline bool kIsNothrowMoveAssignable =
      std::is_nothrow_move_constructible_v<Value> &&
      std::is_nothrow_move_assignable_v<Value>;

 public:
  using value_type = Value;

  using promise_type = detail::OptionalPromise<Value>;

  static_assert(
      !std::is_reference<Value>::value,
      "Optional may not be used with reference types");
  static_assert(
      !std::is_abstract<Value>::value,
      "Optional may not be used with abstract types");

  /// Default-constructed Optionals are None.
  constexpr Optional() noexcept {}

  Optional(const Optional&) //
    requires std::is_copy_constructible_v<Value>
  = default;

  Optional(Optional&& src) noexcept(std::is_nothrow_move_constructible_v<Value>)
    requires std::is_move_constructible_v<Value>
  {
    if (src.hasValue()) {
      this->construct(std::move(src.value()));
      src.reset();
    }
  }

  constexpr /* implicit */ Optional(const None&) noexcept {}

  constexpr /* implicit */ Optional(Value&& newValue) noexcept(
      std::is_nothrow_move_constructible<Value>::value) {
    this->construct(std::move(newValue));
  }

  constexpr /* implicit */ Optional(const Value& newValue) noexcept(
      std::is_nothrow_copy_constructible<Value>::value) {
    this->construct(newValue);
  }

  /**
   * Creates an Optional with a value, where that value is constructed in-place.
   *
   * The std::in_place argument exists so that values can be default constructed
   * (i.e. have no arguments), since this would otherwise be confused with
   * default-constructing an Optional, which in turn results in None.
   */
  template <typename... Args>
  constexpr explicit Optional(std::in_place_t, Args&&... args) noexcept(
      std::is_nothrow_constructible<Value, Args...>::value)
      : Optional{PrivateConstructor{}, std::forward<Args>(args)...} {}

  template <typename U, typename... Args>
  constexpr explicit Optional(
      std::in_place_t,
      std::initializer_list<U> il,
      Args&&... args) noexcept(std::
                                   is_nothrow_constructible<
                                       Value,
                                       std::initializer_list<U>,
                                       Args...>::value)
      : Optional{PrivateConstructor{}, il, std::forward<Args>(args)...} {}

  // Used only when an Optional is used with coroutines on MSVC
  /* implicit */ Optional(const detail::OptionalPromiseReturn<Value>& p)
      : Optional{} {
    p.promise_->value_ = this;
  }

  // Conversions to ease migration to std::optional

  /// Allow construction of Optional from std::optional.
  template <typename U>
    requires std::same_as<U, std::optional<Value>>
  explicit Optional(U&& newValue) noexcept(
      std::is_nothrow_move_constructible<Value>::value) {
    if (newValue.has_value()) {
      this->construct(std::move(*newValue));
      newValue.reset();
    }
  }
  template <typename U>
    requires std::same_as<U, std::optional<Value>>
  explicit Optional(const U& newValue) noexcept(
      std::is_nothrow_copy_constructible<Value>::value) {
    if (newValue.has_value()) {
      this->construct(*newValue);
    }
  }
  /// Allow explicit cast to std::optional
  /// @methodset Migration
  explicit operator std::optional<Value>() && noexcept(
      std::is_nothrow_move_constructible<Value>::value) {
    std::optional<Value> ret = this->storage_.hasValue
        ? std::optional<Value>(std::move(this->storage_.value))
        : std::nullopt;
    reset();
    return ret;
  }
  explicit operator std::optional<Value>() const& noexcept(
      std::is_nothrow_copy_constructible<Value>::value) {
    return this->storage_.hasValue
        ? std::optional<Value>(this->storage_.value)
        : std::nullopt;
  }

  std::optional<Value> toStdOptional() && noexcept {
    if (has_value()) {
      auto opt = std::optional<Value>(std::move(value()));
      reset();
      return opt;
    }
    return std::nullopt;
  }

  std::optional<Value> toStdOptional() const& noexcept {
    if (has_value()) {
      return std::optional<Value>(value());
    }
    return std::nullopt;
  }

  /// Set the Optional
  /// @methodset Modifiers
  void assign(const None&) { reset(); }

  void assign(Optional&& src) {
    if (this != &src) {
      if (src.hasValue()) {
        assign(std::move(src.value()));
        src.reset();
      } else {
        reset();
      }
    }
  }

  void assign(const Optional& src) {
    if (src.hasValue()) {
      assign(src.value());
    } else {
      reset();
    }
  }

  void assign(Value&& newValue) {
    if (hasValue()) {
      FOLLY_PUSH_WARNING
      FOLLY_GCC_DISABLE_WARNING("-Wmaybe-uninitialized")
      this->storage_.value = std::move(newValue);
      FOLLY_POP_WARNING
    } else {
      this->construct(std::move(newValue));
    }
  }

  void assign(const Value& newValue) {
    if (hasValue()) {
      FOLLY_PUSH_WARNING
      FOLLY_GCC_DISABLE_WARNING("-Wmaybe-uninitialized")
      this->storage_.value = newValue;
      FOLLY_POP_WARNING
    } else {
      this->construct(newValue);
    }
  }

  /// @methodset Modifiers
  Optional& operator=(None) noexcept {
    reset();
    return *this;
  }

  template <class Arg>
    requires(!std::same_as<std::remove_cvref_t<Arg>, Optional>)
  Optional& operator=(Arg&& arg) {
    assign(std::forward<Arg>(arg));
    return *this;
  }

  Optional& operator=(Optional&& other) noexcept(kIsNothrowMoveAssignable)
    requires kIsMoveAssignable
  {
    assign(std::move(other));
    return *this;
  }

  Optional& operator=(const Optional&)
    requires kIsCopyAssignable
  = default;

  /// Construct a new value in the Optional, in-place.
  /// @methodset Modifiers
  template <class... Args>
  Value& emplace(Args&&... args) {
    reset();
    this->construct(std::forward<Args>(args)...);
    return value();
  }

  template <class U, class... Args>
  typename std::enable_if<
      std::is_constructible<Value, std::initializer_list<U>&, Args&&...>::value,
      Value&>::type
  emplace(std::initializer_list<U> ilist, Args&&... args) {
    reset();
    this->construct(ilist, std::forward<Args>(args)...);
    return value();
  }

  /// Set the Optional to None
  /// @methodset Modifiers
  void reset() noexcept { this->storage_.clear(); }

  /// Set the Optional to None
  /// @methodset Modifiers
  void clear() noexcept { reset(); }

  /// @methodset Modifiers
  void swap(Optional& that) noexcept(std::is_nothrow_swappable_v<Value>) {
    if (hasValue() && that.hasValue()) {
      using std::swap;
      swap(value(), that.value());
    } else if (hasValue()) {
      that.emplace(std::move(value()));
      reset();
    } else if (that.hasValue()) {
      emplace(std::move(that.value()));
      that.reset();
    }
  }

  /// Get the value. Must have value.
  /// @methodset Getters
  constexpr const Value& value() const& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    require_value();
    return this->storage_.value;
  }

  constexpr Value& value() & [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    require_value();
    return this->storage_.value;
  }

  constexpr Value&& value() && [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    require_value();
    return std::move(this->storage_.value);
  }

  constexpr const Value&& value() const&& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    require_value();
    return std::move(this->storage_.value);
  }

  /// Get the value by pointer; nullptr if None.
  /// @methodset Getters
  const Value* get_pointer() const& {
    return this->storage_.hasValue ? &this->storage_.value : nullptr;
  }
  Value* get_pointer() & {
    return this->storage_.hasValue ? &this->storage_.value : nullptr;
  }
  Value* get_pointer() && = delete;

  /// Does this Optional have a value.
  /// @methodset Observers
  constexpr bool has_value() const noexcept { return this->storage_.hasValue; }

  /// Does this Optional have a value.
  /// @methodset Observers
  constexpr bool hasValue() const noexcept { return has_value(); }

  /// Does this Optional have a value.
  /// @methodset Observers
  constexpr explicit operator bool() const noexcept { return has_value(); }

  /// Get the value. Must have value.
  /// @methodset Getters
  constexpr const Value& operator*() const& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return value();
  }
  constexpr Value& operator*() & [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return value();
  }
  constexpr const Value&& operator*()
      const&& [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::move(value());
  }
  constexpr Value&& operator*() && [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return std::move(value());
  }

  /// Get the value. Must have value.
  /// @methodset Getters
  constexpr const Value* operator->() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return &value();
  }
  constexpr Value* operator->() [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return &value();
  }

  /// Return a copy of the value if set, or a given default if not.
  /// @methodset Getters
  template <class U>
  constexpr Value value_or(U&& dflt) const& {
    if (this->storage_.hasValue) {
      return this->storage_.value;
    }

    return std::forward<U>(dflt);
  }

  template <class U>
  constexpr Value value_or(U&& dflt) && {
    if (this->storage_.hasValue) {
      return std::move(this->storage_.value);
    }

    return std::forward<U>(dflt);
  }

 private:
  friend struct detail::OptionalPromiseReturn<Value>;

  template <class T>
  friend constexpr Optional<std::decay_t<T>> make_optional(T&&);
  template <class T, class... Args>
  friend constexpr Optional<T> make_optional(Args&&... args);
  template <class T, class U, class... As>
  friend constexpr Optional<T> make_optional(std::initializer_list<U>, As&&...);

  /**
   * Construct the optional in place, this is duplicated as a non-explicit
   * constructor to allow returning values that are non-movable from
   * make_optional using list initialization.
   *
   * Until C++17, at which point this will become unnecessary because of
   * specified prvalue elision.
   */
  struct PrivateConstructor {
    explicit PrivateConstructor() = default;
  };
  template <typename... Args>
  constexpr Optional(PrivateConstructor, Args&&... args) noexcept(
      std::is_nothrow_constructible<Value, Args&&...>::value) {
    this->construct(std::forward<Args>(args)...);
  }
  // for when coroutine promise return-object conversion is eager
  explicit Optional(detail::OptionalEmptyTag, Optional*& pointer) noexcept {
    pointer = this;
  }

  void require_value() const {
    if (!this->storage_.hasValue) {
      throw_exception<OptionalEmptyException>();
    }
  }
};

template <class T>
const T* get_pointer(const Optional<T>& opt) {
  return opt.get_pointer();
}

template <class T>
T* get_pointer(Optional<T>& opt) {
  return opt.get_pointer();
}

template <class T>
void swap(Optional<T>& a, Optional<T>& b) noexcept(noexcept(a.swap(b))) {
  a.swap(b);
}

template <class T>
constexpr Optional<std::decay_t<T>> make_optional(T&& v) {
  using PrivateConstructor =
      typename folly::Optional<std::decay_t<T>>::PrivateConstructor;
  return {PrivateConstructor{}, std::forward<T>(v)};
}

template <class T, class... Args>
constexpr folly::Optional<T> make_optional(Args&&... args) {
  using PrivateConstructor = typename folly::Optional<T>::PrivateConstructor;
  return {PrivateConstructor{}, std::forward<Args>(args)...};
}

template <class T, class U, class... Args>
constexpr folly::Optional<T> make_optional(
    std::initializer_list<U> il, Args&&... args) {
  using PrivateConstructor = typename folly::Optional<T>::PrivateConstructor;
  return {PrivateConstructor{}, il, std::forward<Args>(args)...};
}

///////////////////////////////////////////////////////////////////////////////
// Comparisons.

template <class U, class V>
constexpr bool operator==(const Optional<U>& a, const V& b) {
  return a.hasValue() && a.value() == b;
}

template <class U, class V>
constexpr bool operator==(const U& a, const Optional<V>& b) {
  return b.hasValue() && b.value() == a;
}

template <class U, class V>
constexpr bool operator==(const Optional<U>& a, const Optional<V>& b) {
  if (a.hasValue() != b.hasValue()) {
    return false;
  }
  if (a.hasValue()) {
    return a.value() == b.value();
  }
  return true;
}

template <class U, class V>
constexpr bool operator<(const Optional<U>& a, const Optional<V>& b) {
  if (a.hasValue() != b.hasValue()) {
    return a.hasValue() < b.hasValue();
  }
  if (a.hasValue()) {
    return a.value() < b.value();
  }
  return false;
}

template <class U, class V>
constexpr bool operator>(const Optional<U>& a, const Optional<V>& b) {
  return b < a;
}

template <class U, class V>
constexpr bool operator<=(const Optional<U>& a, const Optional<V>& b) {
  return !(b < a);
}

template <class U, class V>
constexpr bool operator>=(const Optional<U>& a, const Optional<V>& b) {
  return !(a < b);
}

// Suppress comparability of Optional<T> with T, despite implicit conversion.
template <class V>
bool operator<(const Optional<V>&, const V& other) = delete;
template <class V>
bool operator<=(const Optional<V>&, const V& other) = delete;
template <class V>
bool operator>=(const Optional<V>&, const V& other) = delete;
template <class V>
bool operator>(const Optional<V>&, const V& other) = delete;
template <class V>
bool operator<(const V& other, const Optional<V>&) = delete;
template <class V>
bool operator<=(const V& other, const Optional<V>&) = delete;
template <class V>
bool operator>=(const V& other, const Optional<V>&) = delete;
template <class V>
bool operator>(const V& other, const Optional<V>&) = delete;

// Comparisons with none
template <class V>
constexpr bool operator==(const Optional<V>& a, None) noexcept {
  return !a.hasValue();
}
template <class V>
constexpr bool operator==(None, const Optional<V>& a) noexcept {
  return !a.hasValue();
}
template <class V>
constexpr std::strong_ordering operator<=>(
    const Optional<V>& a, None) noexcept {
  return a.hasValue() <=> false;
}

///////////////////////////////////////////////////////////////////////////////

} // namespace folly

// Allow usage of Optional<T> in std::unordered_map and std::unordered_set
FOLLY_NAMESPACE_STD_BEGIN
template <class T>
struct hash<
    folly::enable_std_hash_helper<folly::Optional<T>, remove_const_t<T>>> {
  size_t operator()(folly::Optional<T> const& obj) const {
    return static_cast<bool>(obj) ? hash<remove_const_t<T>>()(*obj) : 0;
  }
};
FOLLY_NAMESPACE_STD_END

// Enable the use of folly::Optional with `co_await`
// Inspired by https://github.com/toby-allsopp/coroutine_monad
#if FOLLY_HAS_COROUTINES
#include <folly/coro/Coroutine.h>

namespace folly {
namespace detail {
template <typename Value>
struct OptionalPromise;

template <typename Value>
struct OptionalPromiseReturn {
  Optional<Value> storage_;
  Optional<Value>*& pointer_;

  /* implicit */ OptionalPromiseReturn(OptionalPromise<Value>& p) noexcept
      : pointer_{p.value_} {
    pointer_ = &storage_;
  }
  OptionalPromiseReturn(OptionalPromiseReturn const&) = delete;
  // letting dtor be trivial makes the coroutine crash
  // TODO: fix clang/llvm codegen
  ~OptionalPromiseReturn() {}
  /* implicit */ operator Optional<Value>() {
    // handle both deferred and eager return-object conversion behaviors
    // see docs for detect_promise_return_object_eager_conversion
    if (folly::coro::detect_promise_return_object_eager_conversion()) {
      assert(!storage_.has_value());
      return Optional{OptionalEmptyTag{}, pointer_}; // eager
    } else {
      return std::move(storage_); // deferred
    }
  }
};

template <typename Value>
struct OptionalPromise {
  Optional<Value>* value_ = nullptr;
  OptionalPromise() = default;
  OptionalPromise(OptionalPromise const&) = delete;
  OptionalPromiseReturn<Value> get_return_object() noexcept { return *this; }
  coro::suspend_never initial_suspend() const noexcept { return {}; }
  coro::suspend_never final_suspend() const noexcept { return {}; }
  template <typename U = Value>
  void return_value(U&& u) {
    *value_ = static_cast<U&&>(u);
  }
  void unhandled_exception() {
    // Technically, throwing from unhandled_exception is underspecified:
    // https://github.com/GorNishanov/CoroutineWording/issues/17
    rethrow_current_exception();
  }
};

template <typename Value>
struct OptionalAwaitable {
  Optional<Value> o_;
  bool await_ready() const noexcept { return o_.hasValue(); }
  Value await_resume() { return std::move(o_.value()); }

  // Explicitly only allow suspension into an OptionalPromise
  template <typename U>
  void await_suspend(
      coro::coroutine_handle<OptionalPromise<U>> h) const noexcept {
    // Abort the rest of the coroutine. resume() is not going to be called
    h.destroy();
  }
};
} // namespace detail

template <typename Value>
detail::OptionalAwaitable<Value>
/* implicit */ operator co_await(Optional<Value> o) {
  return {std::move(o)};
}
} // namespace folly

#endif // FOLLY_HAS_COROUTINES
