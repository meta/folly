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

#include <folly/CppAttributes.h>
#include <folly/Traits.h>
#include <folly/Utility.h>

namespace folly {

/***
 *  Indestructible
 *
 *  When you need a Meyers singleton that will not get destructed, even at
 *  shutdown, and you also want the object stored inline.
 *
 *  Use like:
 *
 *      void doSomethingWithExpensiveData();
 *
 *      void doSomethingWithExpensiveData() {
 *        static const Indestructible<map<string, int>> data{
 *          map<string, int>{{"key1", 17}, {"key2", 19}, {"key3", 23}},
 *        };
 *        callSomethingTakingAMapByRef(*data);
 *      }
 *
 *  This should be used only for Meyers singletons, and, even then, only when
 *  the instance does not need to be destructed ever.
 *
 *  This should not be used more generally, e.g., as member fields, etc.
 *
 *  This is designed as an alternative, but with one fewer allocation at
 *  construction time and one fewer pointer dereference at access time, to the
 *  Meyers singleton pattern of:
 *
 *    void doSomethingWithExpensiveData() {
 *      static const auto data =  // never `delete`d
 *          new map<string, int>{{"key1", 17}, {"key2", 19}, {"key3", 23}};
 *      callSomethingTakingAMapByRef(*data);
 *    }
 */

struct factory_constructor_t {
  explicit factory_constructor_t() = default;
};

constexpr factory_constructor_t factory_constructor{};

template <typename T>
class Indestructible final {
 public:
  template <typename S = T, typename = decltype(S())>
  constexpr Indestructible() noexcept(noexcept(T()))
      : storage_{std::in_place} {}

  /**
   * Constructor accepting a single argument by forwarding reference, this
   * allows using list initialization without the overhead of things like
   * std::in_place, etc and also works with std::initializer_list constructors
   * which can't be deduced, the default parameter helps there.
   *
   *    auto i = folly::Indestructible<std::map<int, int>>{{{1, 2}}};
   *
   * This provides convenience
   *
   * The constructor is implicit when the element is implicitly constructible
   * from the given argument and explicit otherwise.
   */
  template <typename U = T>
    requires(
        std::is_constructible_v<T, U &&> &&
        !std::is_same_v<Indestructible<T>, std::remove_cvref_t<U>>)
  explicit(!std::is_convertible_v<U&&, T>) constexpr Indestructible(
      U&& u) noexcept(noexcept(T(std::declval<U>())))
      : storage_{std::in_place, std::forward<U>(u)} {}

  template <typename... Args, typename = decltype(T(std::declval<Args>()...))>
  explicit constexpr Indestructible(Args&&... args) noexcept(
      noexcept(T(std::declval<Args>()...)))
      : storage_{std::in_place, std::forward<Args>(args)...} {}
  template <
      typename U,
      typename... Args,
      typename = decltype(T(
          std::declval<std::initializer_list<U>&>(), std::declval<Args>()...))>
  explicit constexpr Indestructible(std::initializer_list<U> il, Args... args) noexcept(
      noexcept(T(
          std::declval<std::initializer_list<U>&>(), std::declval<Args>()...)))
      : storage_{std::in_place, il, std::forward<Args>(args)...} {}

  template <typename Factory>
  constexpr Indestructible(factory_constructor_t, Factory&& factory) noexcept(
      noexcept(factory()))
      : storage_(factory_constructor, std::forward<Factory>(factory)) {}

  Indestructible(Indestructible const&) = delete;
  Indestructible& operator=(Indestructible const&) = delete;

  T* get() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return reinterpret_cast<T*>(&storage_.bytes);
  }
  T const* get() const noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return reinterpret_cast<T const*>(&storage_.bytes);
  }
  T& operator*() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return *get(); }
  T const& operator*() const noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return *get();
  }
  T* operator->() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return get(); }
  T const* operator->() const noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return get();
  }

  /* implicit */ operator T&() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return *get();
  }
  /* implicit */ operator T const&() const noexcept
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return *get();
  }

 private:
  struct Storage {
    aligned_storage_for_t<T> bytes;

    template <typename... Args, typename = decltype(T(std::declval<Args>()...))>
    explicit constexpr Storage(std::in_place_t, Args&&... args) noexcept(
        noexcept(T(std::declval<Args>()...))) {
      ::new (&bytes) T(std::forward<Args>(args)...);
    }

    template <typename Factory>
    constexpr Storage(factory_constructor_t, Factory factory) noexcept(
        noexcept(factory())) {
      ::new (&bytes) T(factory());
    }
  };

  Storage storage_{};
};
} // namespace folly
