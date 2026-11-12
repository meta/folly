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

namespace folly {

/**
 * MaybeManagedPtr stores either a raw pointer or a shared_ptr. It provides
 * normal pointer operations on the underlying raw pointer/shared_ptr.
 *
 * When storing a raw pointer, MaybeManagedPtr does not manage the pointer's
 * lifetime, i.e. never calls `free` on the pointer.
 *
 * When storing a shared_ptr, MaybeManagedPtr will release the shared_ptr
 * upon its own destruction.
 */
template <typename T>
class MaybeManagedPtr {
 public:
  /* implicit */ MaybeManagedPtr(T* t)
      : t_(std::shared_ptr<T>(std::shared_ptr<void>(), t)) {}
  /* implicit */ MaybeManagedPtr(std::shared_ptr<T> t) : t_(std::move(t)) {}

  /**
   * Construction from shared_ptr<X>, where X is derived from T.
   */
  template <typename Q>
    requires std::is_base_of_v<T, Q>
  /* implicit */ MaybeManagedPtr(std::shared_ptr<Q> q) : t_(std::move(q)) {}

  /**
   * Get pointer to the element contained in MaybeManagedPtr.
   *
   * @return             Pointer to the element contained in MaybeManagedPtr.
   */
  [[nodiscard]] T* get() const { return t_.get(); }

  /**
   * Return use count of the underlying shared pointer.
   *
   * @return             Use count of the underlying shared pointer.
   */
  [[nodiscard]] long useCount() const { return t_.use_count(); }

  /**
   * Member of pointer operator
   *
   * @return             Pointer to the element contained in MaybeManagedPtr.
   */
  constexpr T* operator->() const { return t_.get(); }

  /**
   * Indirection operator
   *
   * @return             Reference to the element contained in MaybeManagedPtr.
   */
  constexpr T& operator*() const& { return *t_.get(); }

  /**
   * Boolean type conversion operator
   *
   * @return             Returns true if the underlying shared pointer is not
   * null.
   */
  operator bool() const { return (t_.get() != nullptr); }

  /**
   * Boolean equal to operator
   *
   * @return             Returns true if the underlying shared pointer is equal
   * to rhs.
   */
  bool operator==(T* rhs) const { return t_.get() == rhs; }

  /**
   * Boolean equal to operator
   *
   * @return             Returns true if the underlying shared pointer is equal
   * to rhs.
   */
  bool operator==(const std::shared_ptr<T>& rhs) const { return t_ == rhs; }

  /**
   * Pointer type conversion operator
   *
   * @return             Returns a pointer to the element contained in
   * MaybeManagedPtr.
   */
  operator T*() const { return t_.get(); }

 private:
  std::shared_ptr<T> t_;
};

} // namespace folly
