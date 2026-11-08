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

#include <folly/Synchronized.h>
#include <folly/observer/detail/ObserverManager.h>

namespace folly {
namespace observer_detail {
template <typename F>
observer::Observer<ResultOfUnwrapSharedPtr<F>> makeObserver(
    F&& creator,
    std::optional<Core::CreatorContext> creatorContext = std::nullopt) {
  if (!creatorContext) {
    creatorContext = Core::CreatorContext::create(std::forward<F>(creator));
  }
  auto core = Core::create(
      [creator_2 = std::forward<F>(creator)]() mutable {
        return std::static_pointer_cast<const void>(creator_2());
      },
      std::move(creatorContext).value());

  ObserverManager::initCore(core);

  return observer::Observer<ResultOfUnwrapSharedPtr<F>>(core);
}

template <typename F>
observer::Observer<ResultOfNoObserverUnwrap<F>> makeObserver(F&& creator) {
  auto creatorContext = Core::CreatorContext::create(std::forward<F>(creator));
  return makeObserver(
      [creator_2 = std::forward<F>(creator)]() mutable {
        return std::make_shared<ResultOfNoObserverUnwrap<F>>(creator_2());
      },
      std::move(creatorContext));
}

template <typename F>
observer::Observer<ResultOfUnwrapSharedPtr<F>> makeValueObserver(
    F&& creator,
    std::optional<Core::CreatorContext> creatorContext = std::nullopt) {
  if (!creatorContext) {
    creatorContext = Core::CreatorContext::create(std::forward<F>(creator));
  }
  return makeObserver(
      [activeValue = std::shared_ptr<const ResultOfUnwrapSharedPtr<F>>(),
       creator_2 = std::forward<F>(creator)]() mutable {
        auto newValue = creator_2();
        if (!activeValue || !(*activeValue == *newValue)) {
          activeValue = newValue;
        }
        return activeValue;
      },
      std::move(creatorContext));
}

template <typename F>
observer::Observer<ResultOfNoObserverUnwrap<F>> makeValueObserver(F&& creator) {
  auto creatorContext = Core::CreatorContext::create(std::forward<F>(creator));
  return makeValueObserver(
      [creator_2 = std::forward<F>(creator)]() mutable {
        return std::make_shared<ResultOfNoObserverUnwrap<F>>(creator_2());
      },
      std::move(creatorContext));
}
} // namespace observer_detail

namespace observer {

template <typename T>
Snapshot<T> Observer<T>::getSnapshot() const noexcept {
  auto data = core_->getData();
  return Snapshot<T>(
      *core_,
      std::static_pointer_cast<const T>(std::move(data.data)),
      data.version,
      data.timeCreated);
}

template <typename T>
Observer<T>::Observer(observer_detail::Core::Ptr core)
    : core_(std::move(core)) {}

template <typename T>
Observer<T> unwrap(Observer<T> o) {
  return o;
}

template <typename T>
Observer<T> unwrapValue(Observer<T> o) {
  return makeValueObserver(std::move(o));
}

template <typename T>
Observer<T> unwrap(Observer<Observer<T>> oo) {
  return makeObserver([oo_2 = std::move(oo)] {
    return oo_2.getSnapshot()->getSnapshot().getShared();
  });
}

template <typename T>
Observer<T> unwrapValue(Observer<Observer<T>> oo) {
  return makeValueObserver([oo_2 = std::move(oo)] {
    return oo_2.getSnapshot()->getSnapshot().getShared();
  });
}

template <typename F>
Observer<observer_detail::ResultOfUnwrapSharedPtr<F>> makeObserver(
    F&& creator) {
  return observer_detail::makeObserver(std::forward<F>(creator));
}

template <typename F>
Observer<observer_detail::ResultOf<F>> makeObserver(F&& creator) {
  return observer_detail::makeObserver(std::forward<F>(creator));
}

template <typename F>
Observer<observer_detail::ResultOfUnwrapObserver<F>> makeObserver(F&& creator) {
  return unwrap(observer_detail::makeObserver(std::forward<F>(creator)));
}

template <typename T>
Observer<T> makeStaticObserver(T value) {
  return makeStaticObserver<T>(std::make_shared<T>(std::move(value)));
}

template <typename T>
Observer<std::decay_t<T>> makeStaticObserver(std::shared_ptr<T> value) {
  return makeObserver([value_2 = std::move(value)] { return value_2; });
}

template <typename T>
AtomicObserver<T>::AtomicObserver(Observer<T> observer)
    : observer_(std::move(observer)) {}

template <typename T>
AtomicObserver<T>::AtomicObserver(const AtomicObserver<T>& other)
    : AtomicObserver(other.observer_) {}

template <typename T>
AtomicObserver<T>::AtomicObserver(AtomicObserver<T>&& other) noexcept
    : AtomicObserver(std::move(other.observer_)) {}

template <typename T>
AtomicObserver<T>& AtomicObserver<T>::operator=(
    const AtomicObserver<T>& other) {
  return *this = other.observer_;
}

template <typename T>
AtomicObserver<T>& AtomicObserver<T>::operator=(
    AtomicObserver<T>&& other) noexcept {
  return *this = std::move(other.observer_);
}

template <typename T>
AtomicObserver<T>& AtomicObserver<T>::operator=(Observer<T> observer) {
  observer_ = std::move(observer);
  cachedVersion_.store(0, std::memory_order_release);
  return *this;
}

template <typename T>
T AtomicObserver<T>::get() const {
  auto version = cachedVersion_.load(std::memory_order_acquire);
  if (observer_.needRefresh(version) ||
      observer_detail::ObserverManager::inManagerThread()) [[unlikely]] {
    std::unique_lock guard{refreshLock_};
    version = cachedVersion_.load(std::memory_order_acquire);
    if (observer_.needRefresh(version) ||
        observer_detail::ObserverManager::inManagerThread()) [[likely]] {
      auto snapshot = *observer_;
      cachedValue_.store(*snapshot, std::memory_order_relaxed);
      cachedVersion_.store(snapshot.getVersion(), std::memory_order_release);
    }
  }
  return cachedValue_.load(std::memory_order_relaxed);
}

template <typename T>
TLObserver<T>::TLObserver(Observer<T> observer)
    : observer_(std::move(observer)) {}

template <typename T>
TLObserver<T>::TLObserver(const TLObserver<T>& other)
    : TLObserver(other.observer_) {}

template <typename T>
TLObserver<T>::TLObserver(TLObserver<T>&& other) noexcept = default;

template <typename T>
const Snapshot<T>& TLObserver<T>::getSnapshotRef() const {
  Snapshot<T>* snapshot = snapshot_.get();
  if (!snapshot) [[unlikely]] {
    snapshot = new Snapshot<T>(observer_.getSnapshot());
    snapshot_.reset(snapshot);
  }
  if (observer_.needRefresh(*snapshot) ||
      observer_detail::ObserverManager::inManagerThread()) {
    *snapshot = observer_.getSnapshot();
  }

  return *snapshot;
}

template <typename T>
ReadMostlyAtomicObserver<T>::ReadMostlyAtomicObserver(Observer<T> observer)
    : observer_(std::move(observer)),
      cachedValue_(**observer_),
      callback_(observer_.addCallback([this](Snapshot<T> snapshot) {
        cachedValue_.store(*snapshot, std::memory_order_relaxed);
      })) {}

template <typename T>
T ReadMostlyAtomicObserver<T>::get() const {
  if (observer_detail::ObserverManager::inManagerThread()) [[unlikely]] {
    return **observer_;
  }
  return cachedValue_.load(std::memory_order_relaxed);
}

struct CallbackHandle::Context {
  Optional<Observer<folly::Unit>> observer;
  Synchronized<bool> canceled{false};
};

inline CallbackHandle::CallbackHandle() {}

template <typename T>
CallbackHandle::CallbackHandle(
    Observer<T> observer, Function<void(Snapshot<T>)> callback) {
  context_ = std::make_shared<Context>();
  context_->observer = makeObserver(
      [observer_2 = std::move(observer),
       callback_2 = std::move(callback),
       context = context_]() mutable {
        auto rCanceled = context->canceled.rlock();
        if (*rCanceled) {
          return folly::unit;
        }
        auto snapshot = *observer_2;
        observer_detail::ObserverManager::DependencyRecorder::
            withDependencyRecordingDisabled([&] {
              callback_2(std::move(snapshot));
            });
        return folly::unit;
      });
}

inline CallbackHandle& CallbackHandle::operator=(
    CallbackHandle&& handle) noexcept {
  cancel();
  context_ = std::move(handle.context_);
  return *this;
}

inline CallbackHandle::~CallbackHandle() {
  cancel();
}

inline void CallbackHandle::cancel() {
  if (!context_) {
    return;
  }
  context_->observer.reset();
  context_->canceled = true;
  context_.reset();
}

template <typename T>
CallbackHandle Observer<T>::addCallback(
    Function<void(Snapshot<T>)> callback) const {
  return CallbackHandle(*this, std::move(callback));
}

template <typename T>
Observer<T> makeValueObserver(Observer<T> observer) {
  return makeValueObserver([observer] {
    return observer.getSnapshot().getShared();
  });
}

template <typename F>
Observer<observer_detail::ResultOf<F>> makeValueObserver(F&& creator) {
  return observer_detail::makeValueObserver(std::forward<F>(creator));
}

template <typename F>
Observer<observer_detail::ResultOfUnwrapObserver<F>> makeValueObserver(
    F&& creator) {
  return unwrapValue(observer_detail::makeObserver(std::forward<F>(creator)));
}

template <typename F>
Observer<observer_detail::ResultOfUnwrapSharedPtr<F>> makeValueObserver(
    F&& creator) {
  return observer_detail::makeValueObserver(std::forward<F>(creator));
}

} // namespace observer
} // namespace folly
