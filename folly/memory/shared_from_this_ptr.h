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

#include <cstring>
#include <memory>
#include <new>
#include <utility>

#include <folly/Traits.h>

namespace folly {

/// shared_from_this_ptr
///
/// Like std::shared_ptr, but one pointer wide rather than two pointers wide.
/// Suitable for types publicly inheritng std::enable_shared_from_this. However,
/// only requires the type publicly to expose member shared_from_this.
///
/// Useful for cases which may hold large numbers of shared-ptr copies to the
/// same managed object, which implements shared-from-this.
///
/// Necessary as v.s. boost::intrusive_ptr for cases which use weak-ptr to the
/// managed objects as well as shared-ptr, and for cases which already use
/// shared-ptr in the interface in a way which is difficult to change.
///
/// TODO: Optimize refcount ops. Likely requires manipulating std::shared_ptr
/// internals in non-portable, library-specific ways.
template <typename T>
class shared_from_this_ptr {
 private:
  using holder = shared_from_this_ptr;
  using shared = std::shared_ptr<T>;

  T* ptr_{};

  static void incr(T& ref) noexcept {
    auto ptr = ref.shared_from_this();
    folly::aligned_storage_for_t<std::shared_ptr<T>> storage;
    ::new (&storage) shared(std::move(ptr));
  }
  static void decr(T& ref) noexcept {
    auto ptr = ref.shared_from_this();
    folly::aligned_storage_for_t<std::shared_ptr<T>> storage;
    std::memcpy(&storage, &ptr, sizeof(ptr));
    reinterpret_cast<shared&>(storage).~shared();
  }
  static void incr(T* ptr) noexcept { !ptr ? void() : incr(*ptr); }
  static void decr(T* ptr) noexcept { !ptr ? void() : decr(*ptr); }

  void assign(T* ptr) noexcept {
    incr(ptr);
    decr(ptr_);
    ptr_ = ptr;
  }

 public:
  using element_type = typename shared::element_type;

  shared_from_this_ptr() = default;
  explicit shared_from_this_ptr(shared&& ptr) noexcept
      : ptr_{std::exchange(ptr, {}).get()} {
    incr(ptr_);
  }
  explicit shared_from_this_ptr(shared const& ptr) noexcept : ptr_{ptr.get()} {
    incr(ptr_);
  }
  shared_from_this_ptr(holder&& that) noexcept
      : ptr_{std::exchange(that.ptr_, {})} {}
  shared_from_this_ptr(holder const& that) noexcept : ptr_{that.ptr_} {
    incr(ptr_);
  }
  ~shared_from_this_ptr() { decr(ptr_); }

  holder& operator=(holder&& that) noexcept {
    if (this != &that) {
      decr(ptr_);
      ptr_ = std::exchange(that.ptr_, {});
    }
    return *this;
  }
  holder& operator=(holder const& that) noexcept {
    assign(that.ptr_);
    return *this;
  }
  holder& operator=(shared&& ptr) noexcept {
    assign(std::exchange(ptr, {}).get());
    return *this;
  }
  holder& operator=(shared const& ptr) noexcept {
    assign(ptr.get());
    return *this;
  }

  explicit operator bool() const noexcept { return !!ptr_; }
  T* get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }

  explicit operator shared() const noexcept {
    return !ptr_ ? nullptr : ptr_->shared_from_this();
  }

  void reset() noexcept { assign(nullptr); }
  void reset(shared const& ptr) noexcept { assign(ptr.get()); }
  void swap(holder& that) noexcept { std::swap(ptr_, that.ptr_); }

  friend bool operator==(holder const& holder, std::nullptr_t) noexcept {
    return holder.ptr_ == nullptr;
  }

  friend void swap(holder& a, holder& b) noexcept { a.swap(b); }
};

} // namespace folly
