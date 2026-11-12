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

#include <folly/Function.h>
#include <folly/concurrency/memory/TLRefCount.h>

namespace folly {

template <typename T, typename RefCount>
class ReadMostlyMainPtr;
template <typename T, typename RefCount>
class ReadMostlyWeakPtr;
template <typename T, typename RefCount>
class ReadMostlySharedPtr;
template <typename RefCount>
class ReadMostlyMainPtrDeleter;

using DefaultRefCount = TLRefCount;

namespace detail {

/// ReadMostlySharedPtrCore
///
/// The shared control block underlying `ReadMostlyMainPtr`,
/// `ReadMostlyWeakPtr`, and `ReadMostlySharedPtr`. It holds the managed
/// object (as a type-erased `std::shared_ptr<const void>`) along with two
/// independent ref-counts: one for outstanding `ReadMostlySharedPtr`s
/// (which keeps the pointee alive) and one for outstanding weak references
/// (which keeps this control block itself alive). `RefCount` is pluggable
/// so that the "shared" count can use a cheap thread-local implementation
/// (see `DefaultRefCount`) rather than a single contended atomic.
template <typename RefCount = DefaultRefCount>
class ReadMostlySharedPtrCore {
 public:
  std::shared_ptr<const void> getShared() { return ptr_; }

  bool incref() { return ++count_ > 0; }

  void decref() {
    if (--count_ == 0) {
      ptr_.reset();

      decrefWeak();
    }
  }

  void increfWeak() {
    auto value = ++weakCount_;
    DCHECK_GT(value, 0);
  }

  void decrefWeak() {
    if (--weakCount_ == 0) {
      delete this;
    }
  }

  size_t useCount() const { return *count_; }

  ~ReadMostlySharedPtrCore() noexcept {
    assert(*count_ == 0);
    assert(*weakCount_ == 0);
  }

 private:
  template <typename T, typename RefCount2>
  friend class folly::ReadMostlyMainPtr;
  friend class ReadMostlyMainPtrDeleter<RefCount>;

  explicit ReadMostlySharedPtrCore(std::shared_ptr<const void> ptr)
      : ptr_(std::move(ptr)) {}

  RefCount count_;
  RefCount weakCount_;
  std::shared_ptr<const void> ptr_;
};

template <typename From, typename To>
concept ptr_convertible = std::is_convertible_v<From*, To*>;

} // namespace detail

/// ReadMostlyMainPtr
///
/// The single owning handle for a read-mostly-managed object. There is
/// exactly one `ReadMostlyMainPtr` per managed object -- it is move-only,
/// never copyable -- and it is the root from which any number of
/// `ReadMostlySharedPtr` (read-side, shared-ownership) and
/// `ReadMostlyWeakPtr` (non-owning) handles are derived, via `getShared()`
/// or by constructing a `ReadMostlySharedPtr`/`ReadMostlyWeakPtr` from it.
///
/// Compared to `std::shared_ptr`/`std::weak_ptr`, where any shared_ptr copy
/// is as good as any other and there is no distinguished owner,
/// `ReadMostlyMainPtr` plays a role closer to `std::unique_ptr`: it is the
/// sole owner and is responsible for eventually releasing the object.
/// Destroying or resetting the `ReadMostlyMainPtr` does not necessarily
/// destroy the pointee immediately -- it releases the main ptr's own
/// reference and switches the underlying `RefCount` to its slower, globally
/// synchronized mode, since outstanding `ReadMostlySharedPtr`s (created
/// while thread-local operation was safe) may still be dropped from other
/// threads. The pointee is destroyed once the last such reference goes away.
///
/// This class is optimized for workloads where the object is read (i.e.
/// copied into `ReadMostlySharedPtr`s) far more often than it is replaced.
/// With `DefaultRefCount` (`TLRefCount`), acquiring a `ReadMostlySharedPtr`
/// increments a thread-local counter rather than a shared atomic, avoiding
/// cache-line contention across reader threads; the cost is pushed onto the
/// comparatively rare `reset()` on the `ReadMostlyMainPtr`, which must
/// reconcile all thread-local counts.
template <typename T, typename RefCount = DefaultRefCount>
class ReadMostlyMainPtr {
 public:
  ReadMostlyMainPtr() {}

  explicit ReadMostlyMainPtr(std::shared_ptr<T> ptr) { reset(std::move(ptr)); }

  ReadMostlyMainPtr(const ReadMostlyMainPtr&) = delete;
  ReadMostlyMainPtr& operator=(const ReadMostlyMainPtr&) = delete;

  ReadMostlyMainPtr(ReadMostlyMainPtr&& other) noexcept {
    *this = std::move(other);
  }

  ReadMostlyMainPtr& operator=(ReadMostlyMainPtr&& other) noexcept {
    std::swap(impl_, other.impl_);
    std::swap(ptrRaw_, other.ptrRaw_);
    return *this;
  }

  bool operator==(const ReadMostlyMainPtr<T, RefCount>& other) const {
    return get() == other.get();
  }

  bool operator==(T* other) const { return get() == other; }

  bool operator==(const ReadMostlySharedPtr<T, RefCount>& other) const {
    return get() == other.get();
  }

  ~ReadMostlyMainPtr() noexcept { reset(); }

  void reset() noexcept {
    if (impl_) {
      ptrRaw_ = nullptr;
      impl_->count_.useGlobal();
      impl_->weakCount_.useGlobal();
      impl_->decref();
      impl_ = nullptr;
    }
  }

  void reset(std::shared_ptr<T> ptr) {
    reset();
    if (ptr) {
      ptrRaw_ = ptr.get();
      impl_ = new detail::ReadMostlySharedPtrCore<RefCount>(std::move(ptr));
    }
  }

  T* get() const { return ptrRaw_; }

  std::shared_ptr<T> getStdShared() const {
    if (impl_) {
      return {impl_->getShared(), ptrRaw_};
    } else {
      return {};
    }
  }

  T& operator*() const { return *get(); }

  T* operator->() const { return get(); }

  ReadMostlySharedPtr<T, RefCount> getShared() const {
    return ReadMostlySharedPtr<T, RefCount>(*this);
  }

  explicit operator bool() const { return impl_ != nullptr; }

 private:
  template <typename U, typename RefCount2>
  friend class ReadMostlyWeakPtr;
  template <typename U, typename RefCount2>
  friend class ReadMostlySharedPtr;
  friend class ReadMostlyMainPtrDeleter<RefCount>;

  detail::ReadMostlySharedPtrCore<RefCount>* impl_{nullptr};
  T* ptrRaw_{nullptr};
};

/// ReadMostlyWeakPtr
///
/// A non-owning reference to a read-mostly-managed object, analogous to
/// `std::weak_ptr`. It may be constructed from a `ReadMostlyMainPtr`, a
/// `ReadMostlySharedPtr`, or another `ReadMostlyWeakPtr`. It keeps the
/// control block (not the pointee) alive, and does not itself prevent the
/// pointee from being destroyed. Call `lock()` to attempt to obtain a
/// `ReadMostlySharedPtr`, which succeeds as long as the owning
/// `ReadMostlyMainPtr` has not yet released its reference.
template <typename T, typename RefCount = DefaultRefCount>
class ReadMostlyWeakPtr {
 public:
  ReadMostlyWeakPtr() {}

  ReadMostlyWeakPtr(const ReadMostlyWeakPtr& other) { *this = other; }

  ReadMostlyWeakPtr(ReadMostlyWeakPtr&& other) noexcept {
    *this = std::move(other);
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlyWeakPtr(const ReadMostlyWeakPtr<T2, RefCount>& other) {
    *this = other;
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlyWeakPtr(ReadMostlyWeakPtr<T2, RefCount>&& other) noexcept {
    *this = std::move(other);
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  explicit ReadMostlyWeakPtr(const ReadMostlyMainPtr<T2, RefCount>& other) {
    *this = other;
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  explicit ReadMostlyWeakPtr(const ReadMostlySharedPtr<T2, RefCount>& other) {
    *this = other;
  }

  ReadMostlyWeakPtr& operator=(const ReadMostlyWeakPtr& other) {
    reset(other.impl_, other.ptrRaw_);
    return *this;
  }

  ReadMostlyWeakPtr& operator=(ReadMostlyWeakPtr&& other) noexcept {
    std::swap(impl_, other.impl_);
    std::swap(ptrRaw_, other.ptrRaw_);
    return *this;
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlyWeakPtr& operator=(const ReadMostlyWeakPtr<T2, RefCount>& other) {
    reset(other.impl_, other.ptrRaw_);
    return *this;
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlyWeakPtr& operator=(
      ReadMostlyWeakPtr<T2, RefCount>&& other) noexcept {
    reset();
    impl_ = std::exchange(other.impl_, nullptr);
    ptrRaw_ = std::exchange(other.ptrRaw_, nullptr);
    return *this;
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlyWeakPtr& operator=(const ReadMostlyMainPtr<T2, RefCount>& mainPtr) {
    reset(mainPtr.impl_, mainPtr.ptrRaw_);
    return *this;
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlyWeakPtr& operator=(
      const ReadMostlySharedPtr<T2, RefCount>& mainPtr) {
    reset(mainPtr.impl_, mainPtr.ptrRaw_);
    return *this;
  }

  ~ReadMostlyWeakPtr() noexcept { reset(nullptr, nullptr); }

  ReadMostlySharedPtr<T, RefCount> lock() {
    return ReadMostlySharedPtr<T, RefCount>(*this);
  }

 private:
  template <typename U, typename RefCount2>
  friend class ReadMostlyWeakPtr;
  template <typename U, typename RefCount2>
  friend class ReadMostlySharedPtr;

  void reset(detail::ReadMostlySharedPtrCore<RefCount>* impl, T* ptrRaw) {
    if (impl_ == impl) {
      return;
    }

    if (impl_) {
      impl_->decrefWeak();
    }
    impl_ = impl;
    ptrRaw_ = ptrRaw;
    if (impl_) {
      impl_->increfWeak();
    }
  }

  detail::ReadMostlySharedPtrCore<RefCount>* impl_{nullptr};
  T* ptrRaw_{nullptr};
};

/// ReadMostlySharedPtr
///
/// A copyable, shared-ownership handle to a read-mostly-managed object,
/// analogous to `std::shared_ptr`. Unlike `std::shared_ptr`, it cannot be
/// constructed directly from a raw pointer or a `std::shared_ptr` -- it must
/// be obtained from a `ReadMostlyMainPtr` (via `getShared()` or a converting
/// constructor/assignment), from a locked `ReadMostlyWeakPtr`, or by
/// copying another `ReadMostlySharedPtr`. As long as one exists, the
/// pointee stays alive, even after the originating `ReadMostlyMainPtr` has
/// been reset or destroyed. Use `getStdShared()` to obtain an interoperable
/// `std::shared_ptr<T>` that keeps the same object alive.
template <typename T, typename RefCount = DefaultRefCount>
class ReadMostlySharedPtr {
 public:
  ReadMostlySharedPtr() {}

  ReadMostlySharedPtr(const ReadMostlySharedPtr& other) { *this = other; }

  ReadMostlySharedPtr(ReadMostlySharedPtr&& other) noexcept {
    *this = std::move(other);
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlySharedPtr(const ReadMostlySharedPtr<T2, RefCount>& other) {
    *this = other;
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlySharedPtr(ReadMostlySharedPtr<T2, RefCount>&& other) noexcept {
    *this = std::move(other);
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  explicit ReadMostlySharedPtr(const ReadMostlyWeakPtr<T2, RefCount>& other) {
    *this = other;
  }

  // Generally, this shouldn't be used.
  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  explicit ReadMostlySharedPtr(const ReadMostlyMainPtr<T2, RefCount>& other) {
    *this = other;
  }

  ReadMostlySharedPtr& operator=(const ReadMostlySharedPtr& other) {
    reset(other.impl_, other.ptrRaw_);
    return *this;
  }

  ReadMostlySharedPtr& operator=(ReadMostlySharedPtr&& other) noexcept {
    std::swap(impl_, other.impl_);
    std::swap(ptrRaw_, other.ptrRaw_);
    return *this;
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlySharedPtr& operator=(
      const ReadMostlySharedPtr<T2, RefCount>& other) {
    reset(other.impl_, other.ptrRaw_);
    return *this;
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlySharedPtr& operator=(
      ReadMostlySharedPtr<T2, RefCount>&& other) noexcept {
    reset();
    impl_ = std::exchange(other.impl_, nullptr);
    ptrRaw_ = std::exchange(other.ptrRaw_, nullptr);
    return *this;
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlySharedPtr& operator=(const ReadMostlyWeakPtr<T2, RefCount>& other) {
    reset(other.impl_, other.ptrRaw_);
    return *this;
  }

  template <typename T2>
    requires detail::ptr_convertible<T2, T>
  ReadMostlySharedPtr& operator=(const ReadMostlyMainPtr<T2, RefCount>& other) {
    reset(other.impl_, other.ptrRaw_);
    return *this;
  }

  ~ReadMostlySharedPtr() noexcept { reset(nullptr, nullptr); }

  bool operator==(const ReadMostlyMainPtr<T, RefCount>& other) const {
    return get() == other.get();
  }

  bool operator==(T* other) const { return get() == other; }

  bool operator==(const ReadMostlySharedPtr<T, RefCount>& other) const {
    return get() == other.get();
  }

  void reset() { reset(nullptr, nullptr); }

  T* get() const { return ptrRaw_; }

  std::shared_ptr<T> getStdShared() const {
    if (impl_) {
      return {impl_->getShared(), ptrRaw_};
    } else {
      return {};
    }
  }

  T& operator*() const { return *get(); }

  T* operator->() const { return get(); }

  size_t use_count() const { return impl_->useCount(); }

  bool unique() const { return use_count() == 1; }

  explicit operator bool() const { return impl_ != nullptr; }

 private:
  template <typename U, typename RefCount2>
  friend class ReadMostlyWeakPtr;
  template <typename U, typename RefCount2>
  friend class ReadMostlySharedPtr;

  void reset(detail::ReadMostlySharedPtrCore<RefCount>* impl, T* ptrRaw) {
    if (impl_ == impl) {
      return;
    }

    if (impl_) {
      impl_->decref();
      impl_ = nullptr;
      ptrRaw_ = nullptr;
    }

    if (impl && impl->incref()) {
      impl_ = impl;
      ptrRaw_ = ptrRaw;
    }
  }

  T* ptrRaw_{nullptr};
  detail::ReadMostlySharedPtrCore<RefCount>* impl_{nullptr};
};

/// ReadMostlyMainPtrDeleter
///
/// Batches the destruction of multiple `ReadMostlyMainPtr`s so that the
/// comparatively expensive global `RefCount` synchronization they require
/// is paid once for the whole batch rather than once per pointer. Collect
/// pointers via `add()`; they are all released together when the deleter
/// itself is destroyed.
template <typename RefCount = DefaultRefCount>
class ReadMostlyMainPtrDeleter {
 public:
  ~ReadMostlyMainPtrDeleter() noexcept {
    RefCount::useGlobal(refCounts_);
    for (auto& decref : decrefs_) {
      decref();
    }
  }

  template <typename T>
  void add(ReadMostlyMainPtr<T, RefCount> ptr) noexcept {
    if (!ptr.impl_) {
      return;
    }

    refCounts_.push_back(&ptr.impl_->count_);
    refCounts_.push_back(&ptr.impl_->weakCount_);
    decrefs_.push_back([impl = ptr.impl_] { impl->decref(); });
    ptr.impl_ = nullptr;
    ptr.ptrRaw_ = nullptr;
  }

 private:
  std::vector<RefCount*> refCounts_;
  std::vector<folly::Function<void()>> decrefs_;
};

template <typename T, typename RefCount>
inline bool operator==(
    const ReadMostlyMainPtr<T, RefCount>& ptr, std::nullptr_t) {
  return ptr.get() == nullptr;
}

template <typename T, typename RefCount>
inline bool operator==(
    const ReadMostlySharedPtr<T, RefCount>& ptr, std::nullptr_t) {
  return ptr.get() == nullptr;
}

} // namespace folly
