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
#include <cstddef>
#include <utility>

namespace folly {
namespace channels {
namespace detail {

template <typename T>
class intrusive_ptr;

/**
 * An intrusive_ptr is like an std::shared_ptr. However, unlike a shared_ptr,
 * the reference count for an intrusive_ptr lives on the object itself. This has
 * two advantages:
 *
 * 1. Each intrusive_ptr is 8 bytes instead of 16 bytes.
 *
 * 2. An intrusive_ptr can be created from a raw pointer/reference, unlike a
 *    shared_ptr.
 *
 * To use intrusive_ptr<T>, ensure that T inherits from IntrusivePtrBase<T>.
 */
template <typename T>
class IntrusivePtrBase {
 protected:
  IntrusivePtrBase() noexcept = default;
  IntrusivePtrBase(const IntrusivePtrBase&) = delete;
  IntrusivePtrBase& operator=(const IntrusivePtrBase&) = delete;
  IntrusivePtrBase(IntrusivePtrBase&&) = delete;
  IntrusivePtrBase& operator=(IntrusivePtrBase&&) = delete;
  ~IntrusivePtrBase() = default;

 private:
  friend class intrusive_ptr<T>;

  mutable std::atomic<size_t> refCount_{0};
};

template <typename T>
class intrusive_ptr {
 public:
  explicit intrusive_ptr(T* ptr) noexcept : ptr_(ptr) {
    if (ptr_) {
      ptr_->refCount_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  intrusive_ptr(const intrusive_ptr& other) noexcept
      : intrusive_ptr(other.ptr_) {}

  intrusive_ptr(intrusive_ptr&& other) noexcept
      : ptr_(std::exchange(other.ptr_, nullptr)) {}

  intrusive_ptr& operator=(const intrusive_ptr& other) noexcept {
    return *this = intrusive_ptr(other);
  }

  intrusive_ptr& operator=(intrusive_ptr&& other) noexcept {
    intrusive_ptr old(std::move(other));
    std::swap(ptr_, old.ptr_);
    return *this;
  }

  ~intrusive_ptr() {
    if (ptr_ && ptr_->refCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
      delete ptr_;
    }
  }

  T* operator->() const noexcept { return ptr_; }

 private:
  T* ptr_;
};

template <typename T, typename... Args>
intrusive_ptr<T> make_intrusive(Args&&... args) {
  return intrusive_ptr<T>(new T(std::forward<Args>(args)...));
}
} // namespace detail
} // namespace channels
} // namespace folly
