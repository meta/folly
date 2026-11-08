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

#include <folly/CppAttributes.h>
#include <folly/concurrency/CoreCachedSharedPtr.h>
#include <folly/observer/Observer.h>
#include <folly/observer/detail/ObserverManager.h>

namespace folly {
namespace observer {

template <typename T>
class CoreCachedObserver {
  struct CoreCachedSnapshot {
    explicit CoreCachedSnapshot(std::shared_ptr<const T> data)
        : data_(std::move(data)) {}

    const T& operator*() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
      return *get();
    }
    const T* operator->() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
      return get();
    }
    const T* get() const [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
      return data_.get();
    }

    std::shared_ptr<const T> getShared() const& { return data_; }
    std::shared_ptr<const T> getShared() && { return std::move(data_); }

   private:
    std::shared_ptr<const T> data_;
  };

 public:
  explicit CoreCachedObserver(Observer<T> observer)
      : observer_(std::move(observer)),
        data_(observer_.getSnapshot().getShared()),
        callback_(observer_.addCallback([this](Snapshot<T> snapshot) {
          data_.reset(std::move(snapshot).getShared());
        })) {}

  // callback_ captures this, so we cannot move it, hence only the copy
  // constructor is defined (moves will fall back to copy).
  CoreCachedObserver(const CoreCachedObserver& r)
      : CoreCachedObserver(r.observer_) {}
  CoreCachedObserver& operator=(const CoreCachedObserver& r) {
    if (&r != this) {
      this->~CoreCachedObserver();
      new (this) CoreCachedObserver(r);
    }
    return *this;
  }

  CoreCachedSnapshot getSnapshot() const {
    if (observer_detail::ObserverManager::inManagerThread()) [[unlikely]] {
      return CoreCachedSnapshot{observer_.getSnapshot().getShared()};
    }
    return CoreCachedSnapshot{data_.get()};
  }
  CoreCachedSnapshot operator*() const { return getSnapshot(); }

  /**
   * Invoke a function with the current observed value. The snapshot is held
   * alive for the duration of the call, preventing read-after-free when
   * accessing members of the observed object.
   *
   * The return type is decayed to prevent accidentally returning a reference
   * into the snapshot's data, which would dangle after the snapshot is
   * destroyed.
   *
   * See Observer::with() for semantics.
   */
  template <typename F>
  std::decay_t<std::invoke_result_t<F, const T&>> with(F&& f) const
      noexcept(noexcept(static_cast<F&&>(f)(std::declval<const T&>()))) {
    auto snapshot = getSnapshot();
    return static_cast<F&&>(f)(*snapshot);
  }

  const Observer<T>& getUnderlyingObserver() const
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] {
    return observer_;
  }

 private:
  Observer<T> observer_;
  AtomicCoreCachedSharedPtr<const T> data_;
  CallbackHandle callback_;
};

/**
 * Same as makeObserver(...), but creates CoreCachedObserver.
 */
template <typename T>
CoreCachedObserver<T> makeCoreCachedObserver(Observer<T> observer) {
  return CoreCachedObserver<T>(std::move(observer));
}

template <typename F>
auto makeCoreCachedObserver(F&& creator) {
  return makeCoreCachedObserver(makeObserver(std::forward<F>(creator)));
}

} // namespace observer
} // namespace folly
