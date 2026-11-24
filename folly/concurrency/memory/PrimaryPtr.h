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
#include <mutex>
#include <type_traits>

#include <folly/Function.h>
#include <folly/futures/Cleanup.h>
#include <folly/futures/Future.h>

#include <glog/logging.h>

namespace folly {

template <typename T>
class EnablePrimaryFromThis;

template <typename T>
class PrimaryPtr;

template <typename T>
class PrimaryPtrRef;

namespace detail {
struct publicallyDerivedFromEnablePrimaryFromThis_fn {
  template <class T>
  void operator()(const EnablePrimaryFromThis<T>&) const {}
};
} // namespace detail

template <class T>
constexpr bool is_enable_master_from_this_v = std::
    is_invocable_v<detail::publicallyDerivedFromEnablePrimaryFromThis_fn, T>;

template <typename T>
using is_enable_master_from_this =
    std::bool_constant<is_enable_master_from_this_v<T>>;

/**
 * EnablePrimaryFromThis provides an object with appropriate access to the
 * functionality of the PrimaryPtr holding this.
 */
template <typename T>
class EnablePrimaryFromThis {
  // initializes members when the PrimaryPtr for this is constructed
  //
  // used by the PrimaryPtr for this, to invoke the EnablePrimaryFromThis base
  // of T, if it exists.
  template <class O, class Master>
    requires is_enable_master_from_this_v<O>
  static void set(EnablePrimaryFromThis<O>* that, Master& m) {
    that->outerPtrWeak_ = m.outerPtrWeak_;
  }

  template <class O, class Master>
    requires(!is_enable_master_from_this_v<O>)
  static void set(O*, Master&) {}

 public:
  // Gets a non-owning reference to the pointer. PrimaryPtr::join() and the
  // PrimaryPtr::cleanup() work do *NOT* wait for outstanding PrimaryPtrRef
  // objects to be released.
  PrimaryPtrRef<T> masterRefFromThis() {
    return PrimaryPtrRef<T>(outerPtrWeak_);
  }

  // Gets a non-owning const reference to the pointer. PrimaryPtr::join() and
  // the PrimaryPtr::cleanup() work do *NOT* wait for outstanding PrimaryPtrRef
  // objects to be released.
  PrimaryPtrRef<const T> masterRefFromThis() const {
    return PrimaryPtrRef<const T>(outerPtrWeak_);
  }

  // Attempts to lock a pointer. Returns null if pointer is not set or if
  // PrimaryPtr::join() was called or the PrimaryPtr::cleanup() task was started
  // (even if the call to PrimaryPtr::join() hasn't returned yet and the
  // PrimaryPtr::cleanup() task has not completed yet).
  std::shared_ptr<T> masterLockFromThis() {
    if (auto outerPtr = outerPtrWeak_.lock()) {
      return *outerPtr;
    }
    return nullptr;
  }

  // Attempts to lock a pointer. Returns null if pointer is not set or if
  // PrimaryPtr::join() was called or the PrimaryPtr::cleanup() task was started
  // (even if the call to PrimaryPtr::join() hasn't returned yet and the
  // PrimaryPtr::cleanup() task has not completed yet).
  std::shared_ptr<T const> masterLockFromThis() const {
    if (!*this) {
      return nullptr;
    }
    if (auto outerPtr = outerPtrWeak_.lock()) {
      return *outerPtr;
    }
    return nullptr;
  }

 private:
  template <class>
  friend class PrimaryPtr;

  std::weak_ptr<std::shared_ptr<T>> outerPtrWeak_;
};

/**
 * PrimaryPtr should be used to achieve deterministic destruction of objects
 * with shared ownership. Once an object is managed by a PrimaryPtr, shared_ptrs
 * can be obtained pointing to that object. However destroying those shared_ptrs
 * will never call the object destructor inline. To destroy the object, join()
 * method must be called on PrimaryPtr or the task returned from cleanup() must
 * be completed, which will wait for all shared_ptrs to be released and then
 * call the object destructor on the caller supplied execution context.
 */
template <typename T>
class PrimaryPtr {
  // retrieves nested cleanup() work from innerPtr_. Called when the PrimaryPtr
  // cleanup() task has finished waiting for outstanding references
  //
  template <class O>
  static folly::SemiFuture<folly::Unit> getCleanup(O* cleanup) {
    if constexpr (is_cleanup_v<O>) {
      return std::move(*cleanup).cleanup();
    } else {
      return folly::makeSemiFuture();
    }
  }

 public:
  PrimaryPtr() = delete;
  template <class T2, class Deleter>
  PrimaryPtr(std::unique_ptr<T2, Deleter> ptr) {
    set(std::move(ptr));
  }

  explicit PrimaryPtr(std::nullptr_t) {}

  ~PrimaryPtr() {
    if (*this) {
      LOG(FATAL) << "PrimaryPtr has to be joined explicitly.";
    }
  }

  PrimaryPtr(PrimaryPtr<T>&&) = default;

  void swap(PrimaryPtr<T>& other) noexcept {
    PrimaryPtr<T> temp = std::move(other);
    other = std::move(*this);
    *this = std::move(temp);
  }

  PrimaryPtr<T> exchange(PrimaryPtr<T>&& newVal) noexcept {
    PrimaryPtr<T> oldVal = std::move(*this);
    *this = std::move(newVal);
    return oldVal;
  }

  explicit operator bool() const { return !!innerPtr_; }

  // Attempts to lock a pointer. Returns null if pointer is not set or if join()
  // was called or the cleanup() task was started (even if the call to join()
  // hasn't returned yet and the cleanup() task has not completed yet).
  std::shared_ptr<T> lock() const {
    if (auto outerPtr = outerPtrWeak_.lock()) {
      return *outerPtr;
    }
    return nullptr;
  }

  // Waits until all the references obtained via lock() are released. Then
  // destroys the object in the current thread.
  // Can not be called concurrently with set().
  void join() {
    if (!*this) {
      return;
    }
    this->cleanup().get();
  }

  // Returns: a SemiFuture that waits until all the references obtained via
  // lock() are released. Then destroys the object on the Executor provided to
  // the SemiFuture.
  //
  // The returned SemiFuture must run to completion before calling set()
  //
  folly::SemiFuture<folly::Unit> cleanup() {
    return folly::makeSemiFuture()
        // clear outerPtrShared_ after cleanup is started
        // to disable further calls to lock().
        // then wait for outstanding references.
        .deferValue([this](folly::Unit) {
          if (!this->outerPtrShared_) {
            LOG(FATAL)
                << "Cleanup already run - lock() was previously disabled.";
          }
          this->outerPtrShared_.reset();
          return std::move(this->unreferenced_);
        })
        // start cleanup tasks
        .deferValue([this](folly::Unit) { return getCleanup(innerPtr_.get()); })
        .defer([this](folly::Try<folly::Unit> r) {
          if (r.hasException()) {
            LOG(FATAL) << "Cleanup actions must be noexcept.";
          }
          this->innerPtr_.reset();
        });
  }

  // Sets the pointer. Can not be called concurrently with lock() or join() or
  // ref() or while the SemiFuture returned from cleanup() is running.
  template <class T2, class Deleter>
  void set(std::unique_ptr<T2, Deleter> ptr) {
    if (*this) {
      LOG(FATAL) << "PrimaryPtr has to be joined before being set.";
    }

    if (!ptr) {
      return;
    }

    auto rawPtr = ptr.get();
    innerPtr_ = std::unique_ptr<T, folly::Function<void(T*)>>{
        ptr.release(),
        [d = ptr.get_deleter(), rawPtr](T*) mutable { d(rawPtr); }};

    auto [referencesPromise, referencesFuture] =
        folly::makePromiseContract<folly::Unit>();
    unreferenced_ = std::move(referencesFuture);

    // The deleter object needs to be copyable in std::shared_ptr on some
    // platform. To work around this limitation we can slightly tweak the
    // semantics of deleter copy constructor and check we always use this
    // object at most once.
    class LastReference {
     public:
      LastReference(Promise<Unit>&& p) : p_(std::move(p)) {}
      LastReference(LastReference&&) = default;
      LastReference(LastReference& other) : LastReference(std::move(other)) {}
      void operator()(T*) {
        DCHECK(!p_.isFulfilled());
        p_.setValue();
      }

     private:
      Promise<Unit> p_;
    };
    auto innerPtrShared = std::shared_ptr<T>(
        innerPtr_.get(), LastReference{std::move(referencesPromise)});

    outerPtrWeak_ = outerPtrShared_ =
        std::make_shared<std::shared_ptr<T>>(innerPtrShared);

    // attaches optional EnablePrimaryFromThis base of innerPtr_ to this
    // PrimaryPtr
    EnablePrimaryFromThis<T>::set(innerPtr_.get(), *this);
  }

  // Gets a non-owning reference to the pointer. join() and the cleanup() work
  // do *NOT* wait for outstanding PrimaryPtrRef objects to be released.
  PrimaryPtrRef<T> ref() const { return PrimaryPtrRef<T>(outerPtrWeak_); }

 private:
  // Making this private for now since non-null PrimaryPtr's must be explicitly
  // joined before destruction.
  PrimaryPtr<T>& operator=(PrimaryPtr<T>&& other) = default;

  template <class>
  friend class EnablePrimaryFromThis;
  friend class PrimaryPtrRef<T>;

  folly::SemiFuture<folly::Unit> unreferenced_;
  std::shared_ptr<std::shared_ptr<T>> outerPtrShared_;
  std::weak_ptr<std::shared_ptr<T>> outerPtrWeak_;
  std::unique_ptr<T, folly::Function<void(T*)>> innerPtr_;
};

template <typename T>
void swap(PrimaryPtr<T>& x, PrimaryPtr<T>& y) noexcept {
  x.swap(y);
}

template <typename T>
PrimaryPtr<T> exchange(PrimaryPtr<T>& x, PrimaryPtr<T>&& newVal) noexcept {
  return x.exchange(std::move(newVal));
}

/**
 * PrimaryPtrRef is a non-owning reference to the pointer. PrimaryPtr::join()
 * and the PrimaryPtr::cleanup() work do *NOT* wait for outstanding
 * PrimaryPtrRef objects to be released.
 */
template <typename T>
class PrimaryPtrRef {
 public:
  PrimaryPtrRef() = default;

  // Attempts to lock a pointer. Returns null if pointer is not set or if
  // join() was called or cleanup() work was started (even if the call to join()
  // hasn't returned yet or the cleanup() work has not completed yet).
  std::shared_ptr<T> lock() const {
    if (auto outerPtr = outerPtrWeak_.lock()) {
      return *outerPtr;
    }
    return nullptr;
  }

 private:
  template <class>
  friend class EnablePrimaryFromThis;
  template <class>
  friend class PrimaryPtr;
  /* implicit */ PrimaryPtrRef(std::weak_ptr<std::shared_ptr<T>> outerPtrWeak)
      : outerPtrWeak_(std::move(outerPtrWeak)) {}

  std::weak_ptr<std::shared_ptr<T>> outerPtrWeak_;
};

} // namespace folly
