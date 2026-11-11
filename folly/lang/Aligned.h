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

#include <cstddef>
#include <type_traits>
#include <utility>

#include <folly/CppAttributes.h>
#include <folly/Utility.h>
#include <folly/lang/Align.h>

namespace folly {

template <typename T, std::size_t Align>
class aligned {
  static_assert(!(Align & (Align - 1)), "alignment not a power of two");
  static_assert(alignof(T) <= Align, "alignment too small");

 public:
  using alignment = index_constant<Align>;
  using value_type = T;

  aligned() = default;
  aligned(aligned const&) = default;
  aligned(aligned&&) = default;
  template <typename S = T>
    requires std::is_copy_constructible<S>::value
  aligned(T const& value) noexcept(std::is_nothrow_copy_constructible<T>::value)
      : value_(value) {}
  template <typename S = T>
    requires std::is_move_constructible<S>::value
  aligned(T&& value) noexcept(std::is_nothrow_move_constructible<T>::value)
      : value_(static_cast<T&&>(value)) {}
  template <typename... A>
    requires std::is_constructible<T, A...>::value
  explicit aligned(std::in_place_t, A&&... a) noexcept(
      std::is_nothrow_constructible<T, A...>::value)
      : value_(static_cast<A&&>(a)...) {}

  aligned& operator=(aligned const&) = default;
  aligned& operator=(aligned&&) = default;
  template <typename S = T>
    requires std::is_copy_assignable<S>::value
  aligned& operator=(T const& value) noexcept(
      std::is_nothrow_copy_assignable<T>::value) {
    value_ = value;
    return *this;
  }
  template <typename S = T>
    requires std::is_move_assignable<S>::value
  aligned& operator=(T&& value) noexcept(
      std::is_nothrow_move_assignable<T>::value) {
    value_ = std::move(value);
    return *this;
  }

  T* get() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return &value_; }
  T const* get() const noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return &value_;
  }
  T* operator->() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return &value_;
  }
  T const* operator->() const noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return &value_;
  }
  T& operator*() noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] { return value_; }
  T const& operator*() const noexcept [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return value_;
  }

 private:
  alignas(Align) T value_;
};

template <typename T>
using cacheline_aligned = aligned<
    T,
    (cacheline_align_v < alignof(T) ? alignof(T) : cacheline_align_v)>;

} // namespace folly
