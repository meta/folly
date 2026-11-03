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

#include <folly/Function.h>

namespace folly {
namespace observer {

namespace detail {

template <typename T, typename O>
concept HasStaticGetName = requires(O& o) {
  { T::getName(o) } -> std::convertible_to<std::string_view>;
};

template <typename Observable, typename Traits>
class ObserverCreatorContext {
  using T = typename Traits::element_type;

 public:
  template <typename... Args>
  ObserverCreatorContext(Args&&... args)
      : observable_(std::forward<Args>(args)...) {
    state_.unsafeGetUnlocked().updateValue(Traits::get(observable_));
  }

  ~ObserverCreatorContext() {
    if (state_.unsafeGetUnlocked().value) {
      Traits::unsubscribe(observable_);
    }
  }

  void setCore(observer_detail::Core::WeakPtr coreWeak) {
    coreWeak_ = std::move(coreWeak);
  }

  std::shared_ptr<const T> get() {
    auto state = state_.wlock();
    state->updateRequested = false;
    return state->value;
  }

  observer_detail::Core::Ptr update() noexcept {
    try {
      // This mutex ensures there's no race condition between initial update()
      // call and update() calls from the subsciption callback.
      //
      // Additionally it helps avoid races between two different subscription
      // callbacks (getting new value from observable and storing it into value_
      // is not atomic).
      //
      // Note that state_ lock is acquired only after Traits::get. Traits::get
      // is running application code (that may acquire locks) and so it's
      // important to not hold state_ lock while running it to avoid possible
      // lock inversion with another code path that needs state_ lock (e.g.
      // get()).
      std::lock_guard updateLockGuard(updateLock_);
      auto newValue = Traits::get(observable_);

      auto state = state_.wlock();
      if (!state->updateValue(std::move(newValue))) {
        // Value didn't change, so we can skip the version update.
        return nullptr;
      }

      if (!std::exchange(state->updateRequested, true)) {
        return coreWeak_.lock();
      }
    } catch (...) {
      LOG(ERROR) << "Observer update failed: "
                 << folly::exceptionStr(current_exception());
    }

    return nullptr;
  }

  template <typename F>
  void subscribe(F&& callback) {
    Traits::subscribe(observable_, std::forward<F>(callback));
  }

  std::string getName() { return std::string{Traits::getName(observable_)}; }

 private:
  mutable SharedMutex updateLock_;
  struct State {
    bool updateValue(std::shared_ptr<const T> newValue) {
      auto newValuePtr = newValue.get();
      if (!newValue) {
        throw std::logic_error("Observable returned nullptr.");
      }
      value.swap(newValue);
      return newValuePtr != newValue.get();
    }

    std::shared_ptr<const T> value;
    bool updateRequested{false};
  };
  folly::Synchronized<State> state_;

  observer_detail::Core::WeakPtr coreWeak_;

  Observable observable_;
};

} // namespace detail

// This master shared_ptr allows grabbing derived weak_ptrs, pointing to the
// the same Context object, but using a separate reference count. Primary
// shared_ptr destructor then blocks until all shared_ptrs obtained from
// derived weak_ptrs are released.
template <typename Observable, typename Traits>
class ObserverCreator<Observable, Traits>::ContextPrimaryPtr {
 public:
  explicit ContextPrimaryPtr(std::shared_ptr<Context> context)
      : contextPrimary_(std::move(context)),
        context_(
            contextPrimary_.get(), [destroyBaton = destroyBaton_](Context*) {
              destroyBaton->post();
            }) {}
  ~ContextPrimaryPtr() {
    if (context_) {
      context_.reset();
      destroyBaton_->wait();
    }
  }
  ContextPrimaryPtr(const ContextPrimaryPtr&) = delete;
  ContextPrimaryPtr(ContextPrimaryPtr&&) = default;
  ContextPrimaryPtr& operator=(const ContextPrimaryPtr&) = delete;
  ContextPrimaryPtr& operator=(ContextPrimaryPtr&&) = default;

  Context* operator->() const { return contextPrimary_.get(); }

  std::weak_ptr<Context> get_weak() { return context_; }

 private:
  std::shared_ptr<folly::Baton<>> destroyBaton_{
      std::make_shared<folly::Baton<>>()};
  std::shared_ptr<Context> contextPrimary_;
  std::shared_ptr<Context> context_;
};

template <typename Observable, typename Traits>
class ObserverCreator<Observable, Traits>::NamedCreator {
 public:
  NamedCreator(
      std::string nameP, folly::Function<std::shared_ptr<const T>()> creatorP)
      : name_(std::move(nameP)), creator_(std::move(creatorP)) {}

  std::shared_ptr<const T> operator()() { return creator_(); }

  const std::string& getName() const { return name_; }

 private:
  std::string name_;
  folly::Function<std::shared_ptr<const T>()> creator_;
};

template <typename Observable, typename Traits>
template <typename... Args>
ObserverCreator<Observable, Traits>::ObserverCreator(Args&&... args)
    : context_(std::make_shared<Context>(std::forward<Args>(args)...)) {}

template <typename Observable, typename Traits>
Observer<typename ObserverCreator<Observable, Traits>::T>
ObserverCreator<Observable, Traits>::getObserver() && {
  // We want to make sure that Context can only be destroyed when Core is
  // destroyed. So we have to avoid the situation when subscribe callback is
  // locking Context shared_ptr and remains the last to release it.
  // We solve this by having Core hold the master shared_ptr and subscription
  // callback gets derived weak_ptr.
  ContextPrimaryPtr contextPrimary(context_);
  auto contextWeak = contextPrimary.get_weak();

  auto observer = [&] {
    if constexpr (detail::HasStaticGetName<Traits, Observable>) {
      auto name = contextPrimary->getName();
      return makeObserver(NamedCreator(
          std::move(name),
          [context = std::move(contextPrimary)]() { return context->get(); }));
    }
    return makeObserver([context = std::move(contextPrimary)]() {
      return context->get();
    });
  }();

  context_->setCore(observer.core_);

  auto scheduleUpdate = [contextWeak_2 = std::move(contextWeak)] {
    observer_detail::ObserverManager::scheduleRefreshNewVersion(
        [contextWeak_2]() -> observer_detail::Core::Ptr {
          if (auto context = contextWeak_2.lock()) {
            return context->update();
          }
          return nullptr;
        });
  };

  context_->subscribe(scheduleUpdate);

  // Do an extra update in case observable was updated between observer
  // creation and setting updates callback.
  scheduleUpdate();

  return observer;
}
} // namespace observer
} // namespace folly
