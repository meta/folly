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

#include <memory>
#include <new>
#include <type_traits>

#include <folly/ScopeGuard.h>

namespace folly {
namespace coro {
namespace detail {

// Helper class for a variable with manually-controlled lifetime.
//
// You must explicitly call .construct() to construct/initialise the value.
//
// If it has been initialised then you must explicitly call .destruct() before
// the ManualLifetime object is destroyed to ensure the destructor is run.
template <typename T>
class ManualLifetime {
 public:
  ManualLifetime() noexcept {}
  ~ManualLifetime() {}

  template <typename... Args>
    requires std::is_constructible_v<T, Args...>
  void construct(Args&&... args) noexcept(
      noexcept(std::is_nothrow_constructible<T, Args...>::value)) {
    ::new (static_cast<void*>(std::addressof(value_)))
        T(static_cast<Args&&>(args)...);
  }

  void destruct() noexcept { value_.~T(); }

  const T& get() const& { return value_; }
  T& get() & { return value_; }
  const T&& get() const&& { return static_cast<const T&&>(value_); }
  T&& get() && { return static_cast<T&&>(value_); }

 private:
  union {
    std::remove_const_t<T> value_;
  };
};

template <typename T>
class ManualLifetime<T&> {
 public:
  ManualLifetime() noexcept : ptr_(nullptr) {}
  ~ManualLifetime() {}

  void construct(T& value) noexcept { ptr_ = std::addressof(value); }

  void destruct() noexcept { ptr_ = nullptr; }

  T& get() const noexcept { return *ptr_; }

 private:
  T* ptr_;
};

template <typename T>
class ManualLifetime<T&&> {
 public:
  ManualLifetime() noexcept : ptr_(nullptr) {}
  ~ManualLifetime() {}

  void construct(T&& value) noexcept { ptr_ = std::addressof(value); }

  void destruct() noexcept { ptr_ = nullptr; }

  T&& get() const noexcept { return static_cast<T&&>(*ptr_); }

 private:
  T* ptr_;
};

template <>
class ManualLifetime<void> {
 public:
  void construct() noexcept {}

  void destruct() noexcept {}

  void get() const noexcept {}
};

// For use when the ManualLifetime is a member of a union. First,
// it in-place constructs the ManualLifetime, making it the active
// member of the union. Then it calls 'construct' on it to construct
// the value inside it.
template <typename T, typename... Args>
  requires std::is_constructible_v<T, Args...>
void activate(ManualLifetime<T>& box, Args&&... args) noexcept(
    std::is_nothrow_constructible<T, Args...>::value) {
  auto* p = ::new (&box) ManualLifetime<T>{};
  // Use ScopeGuard to destruct the ManualLifetime if the 'construct' throws.
  auto guard = makeGuard([p]() noexcept { p->~ManualLifetime(); });
  p->construct(static_cast<Args&&>(args)...);
  guard.dismiss();
}

// For use when the ManualLifetime is a member of a union. First,
// it calls 'destruct' on the ManualLifetime to destroy the value
// inside it. Then it calls the destructor of the ManualLifetime
// object itself.
template <typename T>
void deactivate(ManualLifetime<T>& box) noexcept {
  box.destruct();
  box.~ManualLifetime();
}

} // namespace detail
} // namespace coro
} // namespace folly
