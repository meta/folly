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

#include <initializer_list>
#include <new>
#include <type_traits>
#include <utility>

#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/Utility.h>

/**
 * An instance of `Replaceable<T>` wraps an instance of `T`.
 *
 * You access the inner `T` instance with `operator*` and `operator->` (as if
 * it were a smart pointer).
 *
 * `Replaceable<T>` adds no indirection cost and performs no allocations.
 *
 * `Replaceable<T>` has the same size and alignment as `T`.
 *
 * You can replace the `T` within a `Replaceable<T>` using the `emplace` method
 * (presuming that it is constructible and destructible without throwing
 * exceptions). If the destructor or constructor you're using could throw an
 * exception you should use `Optional<T>` instead, as it's not a logic error
 * for that to be empty.
 *
 * Frequently Asked Questions
 * ==========================
 *
 * Why does this need to be so complicated?
 * ----------------------------------------
 *
 * If a `T` instance contains `const`-qualified member variables or reference
 * member variables we can't safely replace a `T` instance by destructing it
 * manually and using placement new. This is because compilers are permitted to
 * assume that the `const` or reference members of a named, referenced, or
 * pointed-to object do not change.
 *
 * For pointed-to objects in allocated storage you can use the pointer returned
 * by placement new or use the `launder` function to get a pointer to the new
 * object.  Note that `launder` doesn't affect its argument, it's still
 * undefined behaviour to use the original pointer. And none of this helps if
 * the object is a local or a member variable because the destructor call will
 * not have been laundered. In summary, this is the only way to use placement
 * new that is both simple and safe:
 *
 *      T* pT = new T(...);
 *      pT->~T();
 *      pT = ::new (pT) T(...);
 *      delete pT;
 *
 * What are the other safe solutions to this problem?
 * --------------------------------------------------
 *
 * * Ask the designer of `T` to de-`const` and -`reference` the members of `T`.
 *  - Makes `T` harder to reason about
 *  - Can reduce the performance of `T` methods
 *  - They can refuse to make the change
 * * Put the `T` on the heap and use a raw/unique/shared pointer.
 *  - Adds a level of indirection, costing performance.
 *  - Harder to reason about your code as you need to check for nullptr.
 * * Put the `T` in an `Optional`.
 *  - Harder to reason about your code as you need to check for None.
 * * Pass the problem on, making the new code also not-replaceable
 *  - Contagion is not really a solution
 *
 * Are there downsides to this?
 * ----------------------------
 *
 * There is a potential performance penalty after converting `T` to
 * `Replaceable<T>` if you have non-`T`-member-function code which repeatedly
 * examines the value of a `const` or `reference` data member of `T`, because
 * the compiler now has to look at the value each time whereas previously it
 * was permitted to load it once up-front and presume that it could never
 * change.
 *
 * Usage notes
 * ===========
 *
 * Don't store a reference to the `T` within a `Replaceable<T>` unless you can
 * show that its lifetime does not cross an `emplace` call. For safety a
 * reasonable rule is to always use `operator*()` to get a fresh temporary each
 * time you need a `T&.
 *
 * If you store a pointer to the `T` within a `Replaceable<T>` you **must**
 * launder it after each call to `emplace` before using it. Again you can
 * reasonably choose to always use `operator->()` to get a fresh temporary each
 * time you need a `T*.
 *
 * Thus far I haven't thought of a good reason to use `Replaceable<T>` or
 * `Replaceable<T> const&` as a function parameter type.
 *
 * `Replaceable<T>&` can make sense to pass to a function that conditionally
 * replaces the `T`, where `T` has `const` or reference member variables.
 *
 * The main use of `Replaceable<T>` is as a class member type or a local type
 * in long-running functions.
 *
 * It's probably time to rethink your design choices if you end up with
 * `Replaceable<Replaceable<T>>`, `Optional<Replaceable<T>>`,
 * `Replaceable<Optional<T>>`, `unique_ptr<Replaceable<T>>` etc. except as a
 *  result of template expansion.
 */

namespace folly {

template <class T>
class Replaceable;

namespace replaceable_detail {
/* Mixin template to give `Replaceable<T>` a trivial destructor if `T` has a
 * trivial destructor, a user-provided one if `T` is otherwise destructible,
 * and a deleted one if `T` is not destructible.
 */
template <
    class T,
    bool = std::is_destructible<T>::value,
    bool = std::is_trivially_destructible<T>::value>
struct dtor_mixin;

/* Destructible and trivially destructible */
template <class T>
struct dtor_mixin<T, true, true> {};

/* Destructible and not trivially destructible */
template <class T>
struct dtor_mixin<T, true, false> {
  dtor_mixin() = default;
  dtor_mixin(dtor_mixin&&) = default;
  dtor_mixin(dtor_mixin const&) = default;
  dtor_mixin& operator=(dtor_mixin&&) = default;
  dtor_mixin& operator=(dtor_mixin const&) = default;
  ~dtor_mixin() noexcept(std::is_nothrow_destructible<T>::value) {
    T* destruct_ptr = std::launder(
        reinterpret_cast<T*>(
            reinterpret_cast<Replaceable<T>*>(this)->storage_));
    destruct_ptr->~T();
  }
};

/* Not destructible */
template <class T, bool A>
struct dtor_mixin<T, false, A> {
  dtor_mixin() = default;
  dtor_mixin(dtor_mixin&&) = default;
  dtor_mixin(dtor_mixin const&) = default;
  dtor_mixin& operator=(dtor_mixin&&) = default;
  dtor_mixin& operator=(dtor_mixin const&) = default;
  ~dtor_mixin() = delete;
};

template <typename T>
struct is_constructible_from_replaceable
    : std::bool_constant<
          std::is_constructible<T, Replaceable<T>&>::value ||
          std::is_constructible<T, Replaceable<T>&&>::value ||
          std::is_constructible<T, const Replaceable<T>&>::value ||
          std::is_constructible<T, const Replaceable<T>&&>::value> {};

template <typename T>
struct is_convertible_from_replaceable
    : std::bool_constant<
          std::is_convertible<Replaceable<T>&, T>::value ||
          std::is_convertible<Replaceable<T>&&, T>::value ||
          std::is_convertible<const Replaceable<T>&, T>::value ||
          std::is_convertible<const Replaceable<T>&&, T>::value> {};
} // namespace replaceable_detail

template <class T>
using is_replaceable = is_instantiation_of<Replaceable, T>;

// Function to make a Replaceable with a type deduced from its input
template <class T>
constexpr Replaceable<std::decay_t<T>> make_replaceable(T&& t) {
  return Replaceable<std::decay_t<T>>(std::forward<T>(t));
}

template <class T, class... Args>
constexpr Replaceable<T> make_replaceable(Args&&... args) {
  return Replaceable<T>(std::in_place, std::forward<Args>(args)...);
}

template <class T, class U, class... Args>
constexpr Replaceable<T> make_replaceable(
    std::initializer_list<U> il, Args&&... args) {
  return Replaceable<T>(std::in_place, il, std::forward<Args>(args)...);
}

template <class T>
class alignas(T) Replaceable : public replaceable_detail::dtor_mixin<T> {
 public:
  using value_type = T;

  Replaceable() noexcept(std::is_nothrow_default_constructible_v<T>)
    requires std::is_default_constructible_v<T>
  {
    ::new (storage_) T();
  }

  Replaceable(const Replaceable& other) noexcept(
      std::is_nothrow_copy_constructible_v<T>)
    requires std::is_copy_constructible_v<T>
  {
    ::new (storage_) T(*other);
  }

  Replaceable(Replaceable&& other) noexcept(
      std::is_nothrow_move_constructible_v<T>)
    requires std::is_move_constructible_v<T>
  {
    ::new (storage_) T(*std::move(other));
  }

  /* These destruct the `T` and then in-place construct a new one based on what
   * is in the other replaceable; they do not invoke the assignment operator of
   * `T`.
   */
  Replaceable& operator=(const Replaceable& other) noexcept(
      std::is_nothrow_destructible_v<T> &&
      std::is_nothrow_copy_constructible_v<T>)
    requires(std::is_destructible_v<T> && std::is_copy_constructible_v<T>)
  {
    std::launder(reinterpret_cast<T*>(storage_))->~T();
    ::new (storage_) T(*other);
    return *this;
  }

  Replaceable& operator=(Replaceable&& other) noexcept(
      std::is_nothrow_destructible_v<T> &&
      std::is_nothrow_move_constructible_v<T>)
    requires(std::is_destructible_v<T> && std::is_move_constructible_v<T>)
  {
    std::launder(reinterpret_cast<T*>(storage_))->~T();
    ::new (storage_) T(*std::move(other));
    return *this;
  }

  ~Replaceable() = default;

  /**
   * Constructors; these are modeled very closely on the definition of
   * `std::optional` in C++17.
   */
  template <class... Args>
    requires(std::is_constructible<T, Args && ...>::value)
  constexpr explicit Replaceable(std::in_place_t, Args&&... args) noexcept(
      std::is_nothrow_constructible<T, Args&&...>::value) {
    ::new (storage_) T(std::forward<Args>(args)...);
  }

  template <class U, class... Args>
    requires(
        std::is_constructible<T, std::initializer_list<U>, Args && ...>::value)
  constexpr explicit Replaceable(
      std::in_place_t, std::initializer_list<U> il, Args&&... args)
      // clang-format off
      noexcept(std::is_nothrow_constructible<
          T,
          std::initializer_list<U>,
          Args&&...>::value) {
    // clang-format on
    ::new (storage_) T(il, std::forward<Args>(args)...);
  }

  template <class U = T>
    requires(
        std::is_constructible<T, U &&>::value &&
        !std::is_same<std::decay_t<U>, std::in_place_t>::value &&
        !std::is_same<Replaceable<T>, std::decay_t<U>>::value &&
        std::is_convertible<U &&, T>::value)
  constexpr /* implicit */ Replaceable(U&& other) noexcept(
      std::is_nothrow_constructible<T, U&&>::value) {
    ::new (storage_) T(std::forward<U>(other));
  }

  template <class U = T>
    requires(
        std::is_constructible<T, U &&>::value &&
        !std::is_same<std::decay_t<U>, std::in_place_t>::value &&
        !std::is_same<Replaceable<T>, std::decay_t<U>>::value &&
        !std::is_convertible<U &&, T>::value)
  constexpr explicit Replaceable(U&& other) noexcept(
      std::is_nothrow_constructible<T, U&&>::value) {
    ::new (storage_) T(std::forward<U>(other));
  }

  template <class U>
    requires(
        std::is_constructible<T, const U&>::value &&
        !replaceable_detail::is_constructible_from_replaceable<T>::value &&
        !replaceable_detail::is_convertible_from_replaceable<T>::value &&
        std::is_convertible<const U&, T>::value)
  /* implicit */ Replaceable(const Replaceable<U>& other) noexcept(
      std::is_nothrow_constructible<T, U const&>::value) {
    ::new (storage_) T(*other);
  }

  template <class U>
    requires(
        std::is_constructible<T, const U&>::value &&
        !replaceable_detail::is_constructible_from_replaceable<T>::value &&
        !replaceable_detail::is_convertible_from_replaceable<T>::value &&
        !std::is_convertible<const U&, T>::value)
  explicit Replaceable(const Replaceable<U>& other) noexcept(
      std::is_nothrow_constructible<T, U const&>::value) {
    ::new (storage_) T(*other);
  }

  template <class U>
    requires(
        std::is_constructible<T, U &&>::value &&
        !replaceable_detail::is_constructible_from_replaceable<T>::value &&
        !replaceable_detail::is_convertible_from_replaceable<T>::value &&
        std::is_convertible<U &&, T>::value)
  /* implicit */ Replaceable(Replaceable<U>&& other) noexcept(
      std::is_nothrow_constructible<T, U&&>::value) {
    ::new (storage_) T(std::move(*other));
  }

  template <class U>
    requires(
        std::is_constructible<T, U &&>::value &&
        !replaceable_detail::is_constructible_from_replaceable<T>::value &&
        !replaceable_detail::is_convertible_from_replaceable<T>::value &&
        !std::is_convertible<U &&, T>::value)
  explicit Replaceable(Replaceable<U>&& other) noexcept(
      std::is_nothrow_constructible<T, U&&>::value) {
    ::new (storage_) T(std::move(*other));
  }

  /**
   * `emplace` destructs the contained object and in-place constructs the
   * replacement.
   *
   * The destructor must not throw (as usual). The constructor must not throw
   * because that would violate the invariant that a `Replaceable<T>` always
   * contains a T instance.
   *
   * As these methods are `noexcept` the program will be terminated if an
   * exception is thrown. If you are encountering this issue you should look at
   * using `Optional` instead.
   */
  template <class... Args>
  T& emplace(Args&&... args) noexcept {
    T* destruct_ptr = std::launder(reinterpret_cast<T*>(storage_));
    destruct_ptr->~T();
    return *::new (storage_) T(std::forward<Args>(args)...);
  }

  template <class U, class... Args>
  T& emplace(std::initializer_list<U> il, Args&&... args) noexcept {
    T* destruct_ptr = std::launder(reinterpret_cast<T*>(storage_));
    destruct_ptr->~T();
    return *::new (storage_) T(il, std::forward<Args>(args)...);
  }

  /**
   * `swap` just calls `swap(T&, T&)`.
   */
  void swap(Replaceable& other) noexcept(
      std::is_nothrow_swappable_v<Replaceable>) {
    using std::swap;
    swap(*(*this), *other);
  }

  /**
   * Methods to access the contained object. Intended to be very unsurprising.
   */
  constexpr const T* operator->() const {
    return std::launder(reinterpret_cast<T const*>(storage_));
  }

  constexpr T* operator->() {
    return std::launder(reinterpret_cast<T*>(storage_));
  }

  constexpr const T& operator*() const& {
    return *std::launder(reinterpret_cast<T const*>(storage_));
  }

  constexpr T& operator*() & {
    return *std::launder(reinterpret_cast<T*>(storage_));
  }

  constexpr T&& operator*() && {
    return std::move(*std::launder(reinterpret_cast<T*>(storage_)));
  }

  constexpr const T&& operator*() const&& {
    return std::move(*std::launder(reinterpret_cast<T const*>(storage_)));
  }

 private:
  friend struct replaceable_detail::dtor_mixin<T>;
  aligned_storage_for_t<T> storage_[1];
};

template <class T>
Replaceable(T) -> Replaceable<T>;

} // namespace folly
