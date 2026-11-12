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

//
// Docs: https://fburl.com/fbcref_synchronized
//

/**
 * This module implements a Synchronized abstraction useful in
 * mutex-based concurrency.
 *
 * The Synchronized<T, Mutex> class is the primary public API exposed by this
 * module.  See folly/docs/Synchronized.md for a more complete explanation of
 * this class and its benefits.
 */

#pragma once

#include <folly/Function.h>
#include <folly/Likely.h>
#include <folly/Preprocessor.h>
#include <folly/SharedMutex.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/container/Foreach.h>
#include <folly/functional/ApplyTuple.h>
#include <folly/synchronization/Lock.h>

#include <glog/logging.h>

#include <array>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>

namespace folly {

namespace detail {

template <typename Mutex>
inline constexpr bool kSynchronizedMutexIsUnique = requires(Mutex& mutex) {
  mutex.lock();
};

template <typename Mutex>
inline constexpr bool kSynchronizedMutexIsShared = requires(Mutex& mutex) {
  mutex.lock_shared();
};

template <typename Mutex>
inline constexpr bool kSynchronizedMutexIsUpgrade = requires(Mutex& mutex) {
  mutex.lock_upgrade();
};

/**
 * An enum to describe the "level" of a mutex.  The supported levels are
 *  Unique - a normal mutex that supports only exclusive locking
 *  Shared - a shared mutex which has shared locking and unlocking functions;
 *  Upgrade - a mutex that has all the methods of the two above along with
 *            support for upgradable locking
 */
enum class SynchronizedMutexLevel { Unknown, Unique, Shared, Upgrade };

template <typename Mutex>
inline constexpr SynchronizedMutexLevel kSynchronizedMutexLevel =
    kSynchronizedMutexIsUpgrade<Mutex>  ? SynchronizedMutexLevel::Upgrade
    : kSynchronizedMutexIsShared<Mutex> ? SynchronizedMutexLevel::Shared
    : kSynchronizedMutexIsUnique<Mutex>
    ? SynchronizedMutexLevel::Unique
    : SynchronizedMutexLevel::Unknown;

enum class SynchronizedMutexMethod { Lock, TryLock };

template <SynchronizedMutexLevel Level, SynchronizedMutexMethod Method>
struct SynchronizedLockPolicy {
  static constexpr SynchronizedMutexLevel level = Level;
  static constexpr SynchronizedMutexMethod method = Method;
};
using SynchronizedLockPolicyExclusive = SynchronizedLockPolicy<
    SynchronizedMutexLevel::Unique,
    SynchronizedMutexMethod::Lock>;
using SynchronizedLockPolicyTryExclusive = SynchronizedLockPolicy<
    SynchronizedMutexLevel::Unique,
    SynchronizedMutexMethod::TryLock>;
using SynchronizedLockPolicyShared = SynchronizedLockPolicy<
    SynchronizedMutexLevel::Shared,
    SynchronizedMutexMethod::Lock>;
using SynchronizedLockPolicyTryShared = SynchronizedLockPolicy<
    SynchronizedMutexLevel::Shared,
    SynchronizedMutexMethod::TryLock>;
using SynchronizedLockPolicyUpgrade = SynchronizedLockPolicy<
    SynchronizedMutexLevel::Upgrade,
    SynchronizedMutexMethod::Lock>;
using SynchronizedLockPolicyTryUpgrade = SynchronizedLockPolicy<
    SynchronizedMutexLevel::Upgrade,
    SynchronizedMutexMethod::TryLock>;

template <SynchronizedMutexLevel>
struct SynchronizedLockType_ {};
template <>
struct SynchronizedLockType_<SynchronizedMutexLevel::Unique> {
  template <typename Mutex>
  using apply = std::unique_lock<Mutex>;
};
template <>
struct SynchronizedLockType_<SynchronizedMutexLevel::Shared> {
  template <typename Mutex>
  using apply = std::shared_lock<Mutex>;
};
template <>
struct SynchronizedLockType_<SynchronizedMutexLevel::Upgrade> {
  template <typename Mutex>
  using apply = upgrade_lock<Mutex>;
};
template <SynchronizedMutexLevel Level, typename MutexType>
using SynchronizedLockType =
    typename SynchronizedLockType_<Level>::template apply<MutexType>;

} // namespace detail

/**
 * SynchronizedBase is a helper parent class for Synchronized<T>.
 *
 * It provides wlock() and rlock() methods for shared mutex types,
 * or lock() methods for purely exclusive mutex types.
 */
template <class Subclass, detail::SynchronizedMutexLevel level>
class SynchronizedBase;

template <class LockedType, class Mutex, class LockPolicy>
class LockedPtrBase;
template <class LockedType, class LockPolicy>
class LockedPtr;

/**
 * SynchronizedBase specialization for shared mutex types.
 *
 * This class provides wlock() and rlock() methods for acquiring the lock and
 * accessing the data.
 */
template <class Subclass>
class SynchronizedBase<Subclass, detail::SynchronizedMutexLevel::Shared> {
 private:
  template <typename T, typename P>
  using LockedPtr_ = ::folly::LockedPtr<T, P>;

 public:
  using LockPolicyExclusive = detail::SynchronizedLockPolicyExclusive;
  using LockPolicyShared = detail::SynchronizedLockPolicyShared;
  using LockPolicyTryExclusive = detail::SynchronizedLockPolicyTryExclusive;
  using LockPolicyTryShared = detail::SynchronizedLockPolicyTryShared;

  using WLockedPtr = LockedPtr_<Subclass, LockPolicyExclusive>;
  using ConstWLockedPtr = LockedPtr_<const Subclass, LockPolicyExclusive>;

  using RLockedPtr = LockedPtr_<Subclass, LockPolicyShared>;
  using ConstRLockedPtr = LockedPtr_<const Subclass, LockPolicyShared>;

  using TryWLockedPtr = LockedPtr_<Subclass, LockPolicyTryExclusive>;
  using ConstTryWLockedPtr = LockedPtr_<const Subclass, LockPolicyTryExclusive>;

  using TryRLockedPtr = LockedPtr_<Subclass, LockPolicyTryShared>;
  using ConstTryRLockedPtr = LockedPtr_<const Subclass, LockPolicyTryShared>;

  // These aliases are deprecated.
  // TODO: Codemod them away.
  using LockedPtr = WLockedPtr;
  using ConstLockedPtr = ConstRLockedPtr;

  /**
   * @brief Acquire an exclusive lock.
   *
   * Acquire an exclusive lock, and return a LockedPtr that can be used to
   * safely access the datum.
   *
   * LockedPtr offers operator -> and * to provide access to the datum.
   * The lock will be released when the LockedPtr is destroyed.
   *
   * @methodset Exclusive lock
   */
  LockedPtr wlock() { return LockedPtr(static_cast<Subclass*>(this)); }
  ConstWLockedPtr wlock() const {
    return ConstWLockedPtr(static_cast<const Subclass*>(this));
  }

  /**
   * @brief Acquire an exclusive lock, or null.
   *
   * Attempts to acquire the lock in exclusive mode.  If acquisition is
   * unsuccessful, the returned LockedPtr will be null.
   *
   * (Use LockedPtr::operator bool() or LockedPtr::isNull() to check for
   * validity.)
   *
   * @methodset Exclusive lock
   */
  TryWLockedPtr tryWLock() {
    return TryWLockedPtr{static_cast<Subclass*>(this)};
  }
  ConstTryWLockedPtr tryWLock() const {
    return ConstTryWLockedPtr{static_cast<const Subclass*>(this)};
  }

  /**
   * @brief Acquire a read lock.
   *
   * The returned LockedPtr will force const access to the data unless the lock
   * is acquired in non-const context and asNonConstUnsafe() is used.
   *
   * @methodset Shared lock
   */
  RLockedPtr rlock() { return RLockedPtr(static_cast<Subclass*>(this)); }
  ConstLockedPtr rlock() const {
    return ConstLockedPtr(static_cast<const Subclass*>(this));
  }

  /**
   * @brief Acquire a read lock, or null.
   *
   * Attempts to acquire the lock in shared mode.  If acquisition is
   * unsuccessful, the returned LockedPtr will be null.
   *
   * (Use LockedPtr::operator bool() or LockedPtr::isNull() to check for
   * validity.)
   *
   * @methodset Shared lock
   */
  TryRLockedPtr tryRLock() {
    return TryRLockedPtr{static_cast<Subclass*>(this)};
  }
  ConstTryRLockedPtr tryRLock() const {
    return ConstTryRLockedPtr{static_cast<const Subclass*>(this)};
  }

  /**
   * Attempts to acquire the lock, or fails if the timeout elapses first.
   * If acquisition is unsuccessful, the returned LockedPtr will be null.
   *
   * (Use LockedPtr::operator bool() or LockedPtr::isNull() to check for
   * validity.)
   *
   * @methodset Exclusive lock
   */
  template <class Rep, class Period>
  LockedPtr wlock(const std::chrono::duration<Rep, Period>& timeout) {
    return LockedPtr(static_cast<Subclass*>(this), timeout);
  }
  template <class Rep, class Period>
  LockedPtr wlock(const std::chrono::duration<Rep, Period>& timeout) const {
    return LockedPtr(static_cast<const Subclass*>(this), timeout);
  }

  /**
   * Attempts to acquire the lock, or fails if the timeout elapses first.
   * If acquisition is unsuccessful, the returned LockedPtr will be null.
   *
   * (Use LockedPtr::operator bool() or LockedPtr::isNull() to check for
   * validity.)
   *
   * @methodset Shared lock
   */
  template <class Rep, class Period>
  RLockedPtr rlock(const std::chrono::duration<Rep, Period>& timeout) {
    return RLockedPtr(static_cast<Subclass*>(this), timeout);
  }
  template <class Rep, class Period>
  ConstRLockedPtr rlock(
      const std::chrono::duration<Rep, Period>& timeout) const {
    return ConstRLockedPtr(static_cast<const Subclass*>(this), timeout);
  }

  /**
   * Invoke a function while holding the lock exclusively.
   *
   * A reference to the datum will be passed into the function as its only
   * argument.
   *
   * This can be used with a lambda argument for easily defining small critical
   * sections in the code.  For example:
   *
   *   auto value = obj.withWLock([](auto& data) {
   *     data.doStuff();
   *     return data.getValue();
   *   });
   *
   * @methodset Exclusive lock
   */
  template <class Function>
  auto withWLock(Function&& function) {
    return function(*wlock());
  }
  template <class Function>
  auto withWLock(Function&& function) const {
    return function(*wlock());
  }

  /**
   * Invoke a function while holding the lock exclusively.
   *
   * This is similar to withWLock(), but the function will be passed a
   * LockedPtr rather than a reference to the data itself.
   *
   * This allows scopedUnlock() to be called on the LockedPtr argument if
   * desired.
   *
   * @methodset Exclusive lock
   */
  template <class Function>
  auto withWLockPtr(Function&& function) {
    return function(wlock());
  }
  template <class Function>
  auto withWLockPtr(Function&& function) const {
    return function(wlock());
  }

  /**
   * Invoke a function while holding an the lock in shared mode.
   *
   * A const reference to the datum will be passed into the function as its
   * only argument.
   *
   * @methodset Shared lock
   */
  template <class Function>
  auto withRLock(Function&& function) const {
    return function(*rlock());
  }

  /**
   * Invoke a function while holding the lock in shared mode.
   *
   * This is similar to withRLock(), but the function will be passed a
   * LockedPtr rather than a reference to the data itself.
   *
   * This allows scopedUnlock() to be called on the LockedPtr argument if
   * desired.
   *
   * @methodset Shared lock
   */
  template <class Function>
  auto withRLockPtr(Function&& function) {
    return function(rlock());
  }

  template <class Function>
  auto withRLockPtr(Function&& function) const {
    return function(rlock());
  }
};

/**
 * SynchronizedBase specialization for upgrade mutex types.
 *
 * This class provides all the functionality provided by the SynchronizedBase
 * specialization for shared mutexes and a ulock() method that returns an
 * upgrade lock RAII proxy
 */
template <class Subclass>
class SynchronizedBase<Subclass, detail::SynchronizedMutexLevel::Upgrade>
    : public SynchronizedBase<
          Subclass,
          detail::SynchronizedMutexLevel::Shared> {
 private:
  template <typename T, typename P>
  using LockedPtr_ = ::folly::LockedPtr<T, P>;

 public:
  using LockPolicyUpgrade = detail::SynchronizedLockPolicyUpgrade;
  using LockPolicyTryUpgrade = detail::SynchronizedLockPolicyTryUpgrade;

  using UpgradeLockedPtr = LockedPtr_<Subclass, LockPolicyUpgrade>;
  using ConstUpgradeLockedPtr = LockedPtr_<const Subclass, LockPolicyUpgrade>;

  using TryUpgradeLockedPtr = LockedPtr_<Subclass, LockPolicyTryUpgrade>;
  using ConstTryUpgradeLockedPtr =
      LockedPtr_<const Subclass, LockPolicyTryUpgrade>;

  /**
   * @brief Acquire an upgrade lock.
   *
   * The returned LockedPtr will have force const access to the data unless the
   * lock is acquired in non-const context and asNonConstUnsafe() is used.
   *
   * @methodset Upgrade lock
   */
  UpgradeLockedPtr ulock() {
    return UpgradeLockedPtr(static_cast<Subclass*>(this));
  }
  ConstUpgradeLockedPtr ulock() const {
    return ConstUpgradeLockedPtr(static_cast<const Subclass*>(this));
  }

  /**
   * @brief Acquire an upgrade lock, or null.
   *
   * Attempts to acquire the lock in upgrade mode.  If acquisition is
   * unsuccessful, the returned LockedPtr will be null.
   *
   * (Use LockedPtr::operator bool() or LockedPtr::isNull() to check for
   * validity.)
   *
   * @methodset Upgrade lock
   */
  TryUpgradeLockedPtr tryULock() {
    return TryUpgradeLockedPtr{static_cast<Subclass*>(this)};
  }

  /**
   * Acquire an upgrade lock and return a LockedPtr that can be used to safely
   * access the datum
   *
   * And the const version
   *
   * @methodset Upgrade lock
   */
  template <class Rep, class Period>
  UpgradeLockedPtr ulock(const std::chrono::duration<Rep, Period>& timeout) {
    return UpgradeLockedPtr(static_cast<Subclass*>(this), timeout);
  }

  /**
   * Invoke a function while holding the lock.
   *
   * A reference to the datum will be passed into the function as its only
   * argument.
   *
   * This can be used with a lambda argument for easily defining small critical
   * sections in the code.  For example:
   *
   *   auto value = obj.withULock([](auto& data) {
   *     data.doStuff();
   *     return data.getValue();
   *   });
   *
   * This is probably not the function you want.  If the intent is to read the
   * data object and determine whether you should upgrade to a write lock then
   * the withULockPtr() method should be called instead, since it gives access
   * to the LockedPtr proxy (which can be upgraded via the
   * moveFromUpgradeToWrite() method)
   *
   * @methodset Upgrade lock
   */
  template <class Function>
  auto withULock(Function&& function) {
    return function(*ulock());
  }
  template <class Function>
  auto withULock(Function&& function) const {
    return function(*ulock());
  }

  /**
   * Invoke a function while holding the lock exclusively.
   *
   * This is similar to withULock(), but the function will be passed a
   * LockedPtr rather than a reference to the data itself.
   *
   * This allows scopedUnlock() and as_lock() to be called on the
   * LockedPtr argument.
   *
   * This also allows you to upgrade the LockedPtr proxy to a write state so
   * that changes can be made to the underlying data
   *
   * @methodset Upgrade lock
   */
  template <class Function>
  auto withULockPtr(Function&& function) {
    return function(ulock());
  }
  template <class Function>
  auto withULockPtr(Function&& function) const {
    return function(ulock());
  }
};

/**
 * SynchronizedBase specialization for non-shared mutex types.
 *
 * This class provides lock() methods for acquiring the lock and accessing the
 * data.
 */
template <class Subclass>
class SynchronizedBase<Subclass, detail::SynchronizedMutexLevel::Unique> {
 private:
  template <typename T, typename P>
  using LockedPtr_ = ::folly::LockedPtr<T, P>;

 public:
  using LockPolicyExclusive = detail::SynchronizedLockPolicyExclusive;
  using LockPolicyTryExclusive = detail::SynchronizedLockPolicyTryExclusive;

  using LockedPtr = LockedPtr_<Subclass, LockPolicyExclusive>;
  using ConstLockedPtr = LockedPtr_<const Subclass, LockPolicyExclusive>;

  using TryLockedPtr = LockedPtr_<Subclass, LockPolicyTryExclusive>;
  using ConstTryLockedPtr = LockedPtr_<const Subclass, LockPolicyTryExclusive>;

  /**
   * @brief Acquire the lock.
   *
   * Return a LockedPtr that can be used to safely access the datum.
   *
   * @methodset Non-shareable lock
   */
  LockedPtr lock() { return LockedPtr(static_cast<Subclass*>(this)); }

  /**
   * Acquire a lock, and return a ConstLockedPtr that can be used to safely
   * access the datum.
   *
   * @methodset Non-shareable lock
   */
  ConstLockedPtr lock() const {
    return ConstLockedPtr(static_cast<const Subclass*>(this));
  }

  /**
   * @brief Acquire the lock, or null.
   *
   * Attempts to acquire the lock in exclusive mode.  If acquisition is
   * unsuccessful, the returned LockedPtr will be null.
   *
   * (Use LockedPtr::operator bool() or LockedPtr::isNull() to check for
   * validity.)
   *
   * @methodset Non-shareable lock
   */
  TryLockedPtr tryLock() { return TryLockedPtr{static_cast<Subclass*>(this)}; }
  ConstTryLockedPtr tryLock() const {
    return ConstTryLockedPtr{static_cast<const Subclass*>(this)};
  }

  /**
   * Attempts to acquire the lock, or fails if the timeout elapses first.
   * If acquisition is unsuccessful, the returned LockedPtr will be null.
   *
   * @methodset Non-shareable lock
   */
  template <class Rep, class Period>
  LockedPtr lock(const std::chrono::duration<Rep, Period>& timeout) {
    return LockedPtr(static_cast<Subclass*>(this), timeout);
  }

  /**
   * Attempts to acquire the lock, or fails if the timeout elapses first.
   * If acquisition is unsuccessful, the returned LockedPtr will be null.
   *
   * @methodset Non-shareable lock
   */
  template <class Rep, class Period>
  ConstLockedPtr lock(const std::chrono::duration<Rep, Period>& timeout) const {
    return ConstLockedPtr(static_cast<const Subclass*>(this), timeout);
  }

  /**
   * Invoke a function while holding the lock.
   *
   * A reference to the datum will be passed into the function as its only
   * argument.
   *
   * This can be used with a lambda argument for easily defining small critical
   * sections in the code.  For example:
   *
   *   auto value = obj.withLock([](auto& data) {
   *     data.doStuff();
   *     return data.getValue();
   *   });
   *
   * @methodset Non-shareable lock
   */
  template <class Function>
  auto withLock(Function&& function) {
    return function(*lock());
  }
  template <class Function>
  auto withLock(Function&& function) const {
    return function(*lock());
  }

  /**
   * Invoke a function while holding the lock exclusively.
   *
   * This is similar to withWLock(), but the function will be passed a
   * LockedPtr rather than a reference to the data itself.
   *
   * This allows scopedUnlock() and as_lock() to be called on the
   * LockedPtr argument.
   *
   * @methodset Non-shareable lock
   */
  template <class Function>
  auto withLockPtr(Function&& function) {
    return function(lock());
  }
  template <class Function>
  auto withLockPtr(Function&& function) const {
    return function(lock());
  }
};

/**
 * `folly::Synchronized` pairs a datum with a mutex. The datum can only be
 * reached through a `LockedPtr`, typically acquired via `.rlock()` or
 * `.wlock()`; the mutex is held for the lifetime of the `LockedPtr`.
 *
 * It is recommended to explicitly open a new nested scope when aquiring
 * a `LockedPtr` object, to help visibly delineate the critical section and to
 * ensure that the `LockedPtr` is destroyed as soon as it is no longer needed.
 *
 * @tparam T  The type of datum to be stored.
 * @tparam Mutex  The mutex type that guards the datum. Must be Lockable.
 *
 * @refcode folly/docs/examples/folly/Synchronized.cpp
 */
template <class T, class Mutex = SharedMutex>
struct Synchronized
    : public SynchronizedBase<
          Synchronized<T, Mutex>,
          detail::kSynchronizedMutexLevel<Mutex>> {
 private:
  using Base = SynchronizedBase<
      Synchronized<T, Mutex>,
      detail::kSynchronizedMutexLevel<Mutex>>;
  static constexpr bool nxCopyCtor{
      std::is_nothrow_copy_constructible<T>::value};
  static constexpr bool nxMoveCtor{
      std::is_nothrow_move_constructible<T>::value};

  // used to disable copy construction and assignment
  class NonImplementedType;

 public:
  using LockedPtr = typename Base::LockedPtr;
  using ConstLockedPtr = typename Base::ConstLockedPtr;
  using DataType = T;
  using MutexType = Mutex;

  /**
   * Default constructor leaves both members call their own default constructor.
   */
  constexpr Synchronized() = default;

 public:
  /**
   * Copy constructor. Enabled only when the data type is copy-constructible.
   *
   * Takes a shared-or-exclusive lock on the source mutex while performing the
   * copy-construction of the destination data from the source data. No lock is
   * taken on the destination mutex.
   *
   * May throw even when the data type is is nothrow-copy-constructible because
   * acquiring a lock may throw.
   *
   * deprecated
   */
  /* implicit */ Synchronized(
      typename std::conditional<
          std::is_copy_constructible<T>::value,
          const Synchronized&,
          NonImplementedType>::type rhs) /* may throw */
      : Synchronized(rhs.copy()) {}

  /**
   * Move-constructs from the source data without locking either the source or
   * the destination mutex.
   *
   * Semantically, assumes that the source object is a true rvalue and therefore
   * that no synchronization is required for accessing it.
   *
   * deprecated
   */
  Synchronized(Synchronized&& rhs) noexcept(nxMoveCtor)
      : Synchronized(std::move(rhs.datum_)) {}

  /**
   * Constructor taking a datum as argument copies it. There is no
   * need to lock the constructing object.
   */
  explicit Synchronized(const T& rhs) noexcept(nxCopyCtor) : datum_(rhs) {}

  /**
   * Constructor taking a datum rvalue as argument moves it. There is no need
   * to lock the constructing object.
   */
  explicit Synchronized(T&& rhs) noexcept(nxMoveCtor)
      : datum_(std::move(rhs)) {}

  /**
   * Lets you construct non-movable types in-place. Use the constexpr
   * instance `in_place` as the first argument.
   */
  template <typename... Args>
  explicit constexpr Synchronized(std::in_place_t, Args&&... args)
      : datum_(std::forward<Args>(args)...) {}

  /**
   * Lets you construct the synchronized object and also pass construction
   * parameters to the underlying mutex if desired
   */
  template <typename... DatumArgs, typename... MutexArgs>
  Synchronized(
      std::piecewise_construct_t,
      std::tuple<DatumArgs...> datumArgs,
      std::tuple<MutexArgs...> mutexArgs)
      : Synchronized{
            std::piecewise_construct,
            std::move(datumArgs),
            std::move(mutexArgs),
            std::make_index_sequence<sizeof...(DatumArgs)>{},
            std::make_index_sequence<sizeof...(MutexArgs)>{}} {}

  /**
   * Copy assignment operator.
   *
   * Enabled only when the data type is copy-constructible and move-assignable.
   *
   * Move-assigns from a copy of the source data.
   *
   * Takes a shared-or-exclusive lock on the source mutex while copying the
   * source data to a temporary. Takes an exclusive lock on the destination
   * mutex while move-assigning from the temporary.
   *
   * This technique consts an extra temporary but avoids the need to take locks
   * on both mutexes together.
   *
   * deprecated
   */
  Synchronized& operator=(
      typename std::conditional<
          std::is_copy_constructible<T>::value &&
              std::is_move_assignable<T>::value,
          const Synchronized&,
          NonImplementedType>::type rhs) {
    return *this = rhs.copy();
  }

  /**
   * Move assignment operator.
   *
   * Takes an exclusive lock on the destination mutex while move-assigning the
   * destination data from the source data. The source mutex is not locked or
   * otherwise accessed.
   *
   * Semantically, assumes that the source object is a true rvalue and therefore
   * that no synchronization is required for accessing it.
   *
   * deprecated
   */
  Synchronized& operator=(Synchronized&& rhs) {
    return *this = std::move(rhs.datum_);
  }

  /**
   * Lock object, assign datum.
   */
  Synchronized& operator=(const T& rhs) {
    if (&datum_ != &rhs) {
      auto guard = LockedPtr{this};
      datum_ = rhs;
    }
    return *this;
  }

  /**
   * Lock object, move-assign datum.
   */
  Synchronized& operator=(T&& rhs) {
    if (&datum_ != &rhs) {
      auto guard = LockedPtr{this};
      datum_ = std::move(rhs);
    }
    return *this;
  }

  /**
   * @brief Acquire some lock.
   *
   * If the mutex is a shared mutex, and the Synchronized instance is const,
   * this acquires a shared lock.  Otherwise this acquires an exclusive lock.
   *
   * In general, prefer using the explicit rlock() and wlock() methods
   * for read-write locks, and lock() for purely exclusive locks.
   *
   * contextualLock() is primarily intended for use in other template functions
   * that do not necessarily know the lock type.
   */
  LockedPtr contextualLock() { return LockedPtr(this); }
  ConstLockedPtr contextualLock() const { return ConstLockedPtr(this); }
  template <class Rep, class Period>
  LockedPtr contextualLock(const std::chrono::duration<Rep, Period>& timeout) {
    return LockedPtr(this, timeout);
  }
  template <class Rep, class Period>
  ConstLockedPtr contextualLock(
      const std::chrono::duration<Rep, Period>& timeout) const {
    return ConstLockedPtr(this, timeout);
  }
  /**
   * @brief Acquire a lock for reading.
   *
   * contextualRLock() acquires a read lock if the mutex type is shared,
   * or a regular exclusive lock for non-shared mutex types.
   *
   * contextualRLock() when you know that you prefer a read lock (if
   * available), even if the Synchronized<T> object itself is non-const.
   */
  ConstLockedPtr contextualRLock() const { return ConstLockedPtr(this); }
  template <class Rep, class Period>
  ConstLockedPtr contextualRLock(
      const std::chrono::duration<Rep, Period>& timeout) const {
    return ConstLockedPtr(this, timeout);
  }

  /**
   * @brief Acquire a LockedPtr with timeout.
   *
   * Attempts to acquire for a given number of milliseconds. If
   * acquisition is unsuccessful, the returned LockedPtr is nullptr.
   *
   * NOTE: This API is deprecated.  Use lock(), wlock(), or rlock() instead.
   * In the future it will be marked with a deprecation attribute to emit
   * build-time warnings, and then it will be removed entirely.
   */
  LockedPtr timedAcquire(unsigned int milliseconds) {
    return LockedPtr(this, std::chrono::milliseconds(milliseconds));
  }

  /**
   * Attempts to acquire for a given number of milliseconds. If
   * acquisition is unsuccessful, the returned ConstLockedPtr is nullptr.
   *
   * NOTE: This API is deprecated.  Use lock(), wlock(), or rlock() instead.
   * In the future it will be marked with a deprecation attribute to emit
   * build-time warnings, and then it will be removed entirely.
   */
  ConstLockedPtr timedAcquire(unsigned int milliseconds) const {
    return ConstLockedPtr(this, std::chrono::milliseconds(milliseconds));
  }

  /**
   * @brief Swap datum.
   *
   * Swaps with another Synchronized. Protected against
   * self-swap. Only data is swapped. Locks are acquired in increasing
   * address order.
   */
  void swap(Synchronized& rhs) {
    if (this == &rhs) {
      return;
    }
    if (this > &rhs) {
      return rhs.swap(*this);
    }
    auto guard1 = LockedPtr{this};
    auto guard2 = LockedPtr{&rhs};

    using std::swap;
    swap(datum_, rhs.datum_);
  }

  /**
   * Swap with another datum. Recommended because it keeps the mutex
   * held only briefly.
   */
  void swap(T& rhs) {
    LockedPtr guard(this);

    using std::swap;
    swap(datum_, rhs);
  }

  /**
   * @brief Exchange datum.
   *
   * Assign another datum and return the original value. Recommended
   * because it keeps the mutex held only briefly.
   */
  T exchange(T&& rhs) {
    swap(rhs);
    return std::move(rhs);
  }

  /**
   * Copies datum to a given target.
   */
  void copyInto(T& target) const {
    ConstLockedPtr guard(this);
    target = datum_;
  }

  /**
   * Returns a fresh copy of the datum.
   */
  T copy() const {
    ConstLockedPtr guard(this);
    return datum_;
  }

  /**
   * @brief Access datum without locking.
   *
   * Returns a reference to the datum without acquiring a lock.
   *
   * Provided as a backdoor for call-sites where it is known safe to be used.
   * For example, when it is known that only one thread has access to the
   * Synchronized instance.
   *
   * To be used with care - this method explicitly overrides the normal safety
   * guarantees provided by the rest of the Synchronized API.
   */
  T& unsafeGetUnlocked() { return datum_; }
  const T& unsafeGetUnlocked() const { return datum_; }

  /**
   * @brief Access underlying mutex_ directly.
   *
   * Provided as a backdoor for call-sites where the lock and unlock are paired
   * in different calls. For example, in fork handlers. Use carefully as the
   * caller is responsible to ensure it is paired with an unlock and there is
   * nothing else in between that tries to implicitly or explicitly acquire the
   * lock again.
   */
  Mutex& unsafeGetMutex() { return mutex_; }

 private:
  template <class LockedType, class MutexType, class LockPolicy>
  friend class folly::LockedPtrBase;
  template <class LockedType, class LockPolicy>
  friend class folly::LockedPtr;

  /**
   * Helper constructors to enable Synchronized for
   * non-default constructible types T.
   * Guards are created in actual public constructors and are alive
   * for the time required to construct the object
   */
  Synchronized(
      const Synchronized& rhs,
      const ConstLockedPtr& /*guard*/) noexcept(nxCopyCtor)
      : datum_(rhs.datum_) {}

  Synchronized(Synchronized&& rhs, const LockedPtr& /*guard*/) noexcept(
      nxMoveCtor)
      : datum_(std::move(rhs.datum_)) {}

  template <
      typename... DatumArgs,
      typename... MutexArgs,
      std::size_t... IndicesOne,
      std::size_t... IndicesTwo>
  Synchronized(
      std::piecewise_construct_t,
      std::tuple<DatumArgs...> datumArgs,
      std::tuple<MutexArgs...> mutexArgs,
      std::index_sequence<IndicesOne...>,
      std::index_sequence<IndicesTwo...>)
      : datum_{std::get<IndicesOne>(std::move(datumArgs))...},
        mutex_{std::get<IndicesTwo>(std::move(mutexArgs))...} {}

  // simulacrum of data members - keep data members in sync!
  // LockedPtr needs offsetof() which is specified only for standard-layout
  // types which Synchronized is not so we define a simulacrum for offsetof
  struct Simulacrum {
    aligned_storage_for_t<DataType> datum_;
    aligned_storage_for_t<MutexType> mutex_;
  };

  // data members - keep simulacrum of data members in sync!
  T datum_;
  mutable Mutex mutex_;
};

/**
 * Deprecated subclass of Synchronized that provides implicit locking
 * via operator->. This is intended to ease migration while preventing
 * accidental use of operator-> in new code.
 */
template <class T, class Mutex = SharedMutex>
struct [[deprecated(
    "use Synchronized and explicit lock(), wlock(), or rlock() instead")]]
ImplicitSynchronized : Synchronized<T, Mutex> {
 private:
  using Base = Synchronized<T, Mutex>;

 public:
  using LockedPtr = typename Base::LockedPtr;
  using ConstLockedPtr = typename Base::ConstLockedPtr;
  using DataType = typename Base::DataType;
  using MutexType = typename Base::MutexType;

  using Base::Base;
  using Base::operator=;

  /**
   * @brief Access the datum under lock.
   *
   * deprecated
   *
   * This accessor offers a LockedPtr. In turn, LockedPtr offers
   * operator-> returning a pointer to T. The operator-> keeps
   * expanding until it reaches a pointer, so syncobj->foo() will lock
   * the object and call foo() against it.
   *
   * NOTE: This API is planned to be deprecated in an upcoming diff.
   * Prefer using lock(), wlock(), or rlock() instead.
   */
  [[deprecated("use explicit lock(), wlock(), or rlock() instead")]] LockedPtr
  operator->() {
    return LockedPtr(this);
  }

  /**
   * deprecated
   *
   * Obtain a ConstLockedPtr.
   *
   * NOTE: This API is planned to be deprecated in an upcoming diff.
   * Prefer using lock(), wlock(), or rlock() instead.
   */
  [[deprecated(
      "use explicit lock(), wlock(), or rlock() instead")]] ConstLockedPtr
  operator->() const {
    return ConstLockedPtr(this);
  }
};

template <class SynchronizedType, class LockPolicy>
class ScopedUnlocker;

namespace detail {
/*
 * A helper alias that resolves to "const T" if the template parameter
 * is a const Synchronized<T>, or "T" if the parameter is not const.
 */
template <class SynchronizedType, bool AllowsConcurrentAccess>
using SynchronizedDataType = typename std::conditional<
    AllowsConcurrentAccess || std::is_const<SynchronizedType>::value,
    typename SynchronizedType::DataType const,
    typename SynchronizedType::DataType>::type;
/*
 * A helper alias that resolves to a ConstLockedPtr if the template parameter
 * is a const Synchronized<T>, or a LockedPtr if the parameter is not const.
 */
template <class SynchronizedType>
using LockedPtrType = typename std::conditional<
    std::is_const<SynchronizedType>::value,
    typename SynchronizedType::ConstLockedPtr,
    typename SynchronizedType::LockedPtr>::type;

template <
    typename Synchronized,
    typename LockFunc,
    typename TryLockFunc,
    typename... Args>
class SynchronizedLocker {
 public:
  using LockedPtr = invoke_result_t<LockFunc&, Synchronized&, const Args&...>;

  template <typename LockFuncType, typename TryLockFuncType, typename... As>
  SynchronizedLocker(
      Synchronized& sync,
      LockFuncType&& lockFunc,
      TryLockFuncType tryLockFunc,
      As&&... as)
      : synchronized{sync},
        lockFunc_{std::forward<LockFuncType>(lockFunc)},
        tryLockFunc_{std::forward<TryLockFuncType>(tryLockFunc)},
        args_{std::forward<As>(as)...} {}

  auto lock() const {
    auto args = std::tuple<const Args&...>{args_};
    return std::apply(lockFunc_, std::tuple_cat(std::tie(synchronized), args));
  }
  auto tryLock() const { return tryLockFunc_(synchronized); }

 private:
  Synchronized& synchronized;
  LockFunc lockFunc_;
  TryLockFunc tryLockFunc_;
  std::tuple<Args...> args_;
};

template <
    typename Synchronized,
    typename LockFunc,
    typename TryLockFunc,
    typename... Args>
auto makeSynchronizedLocker(
    Synchronized& synchronized,
    LockFunc&& lockFunc,
    TryLockFunc&& tryLockFunc,
    Args&&... args) {
  using LockFuncType = std::decay_t<LockFunc>;
  using TryLockFuncType = std::decay_t<TryLockFunc>;
  return SynchronizedLocker<
      Synchronized,
      LockFuncType,
      TryLockFuncType,
      std::decay_t<Args>...>{
      synchronized,
      std::forward<LockFunc>(lockFunc),
      std::forward<TryLockFunc>(tryLockFunc),
      std::forward<Args>(args)...};
}

/**
 * Acquire locks for multiple Synchronized<T> objects, in a deadlock-safe
 * manner.
 *
 * The function uses the "smart and polite" algorithm from this link
 * http://howardhinnant.github.io/dining_philosophers.html#Polite
 *
 * The gist of the algorithm is that it locks a mutex, then tries to lock the
 * other mutexes in a non-blocking manner.  If all the locks succeed, we are
 * done, if not, we release the locks we have held, yield to allow other
 * threads to continue and then block on the mutex that we failed to acquire.
 *
 * This allows dynamically yielding ownership of all the mutexes but one, so
 * that other threads can continue doing work and locking the other mutexes.
 * See the benchmarks in folly/test/SynchronizedBenchmark.cpp for more.
 */
template <typename... SynchronizedLocker>
auto lock(SynchronizedLocker... lockersIn)
    -> std::tuple<typename SynchronizedLocker::LockedPtr...> {
  // capture the list of lockers as a tuple
  auto lockers = std::forward_as_tuple(lockersIn...);

  // make a list of null LockedPtr instances that we will return to the caller
  auto lockedPtrs = std::tuple<typename SynchronizedLocker::LockedPtr...>{};

  // start by locking the first thing in the list
  std::get<0>(lockedPtrs) = std::get<0>(lockers).lock();
  auto indexLocked = 0;

  while (true) {
    auto couldLockAll = true;

    for_each(lockers, [&](auto& locker, auto index) {
      // if we should try_lock on the current locker then do so
      if (index != indexLocked) {
        auto lockedPtr = locker.tryLock();

        // if we were unable to lock this mutex,
        //
        // 1. release all the locks,
        // 2. yield control to another thread to be nice
        // 3. block on the mutex we failed to lock, acquire the lock
        // 4. break out and set the index of the current mutex to indicate
        //    which mutex we have locked
        if (!lockedPtr) {
          // writing lockedPtrs = decltype(lockedPtrs){} does not compile on
          // gcc, I believe this is a bug D7676798
          lockedPtrs = std::tuple<typename SynchronizedLocker::LockedPtr...>{};

          std::this_thread::yield();
          fetch(lockedPtrs, index) = locker.lock();
          indexLocked = index;
          couldLockAll = false;

          return loop_break;
        }

        // else store the locked mutex in the list we return
        fetch(lockedPtrs, index) = std::move(lockedPtr);
      }

      return loop_continue;
    });

    if (couldLockAll) {
      return lockedPtrs;
    }
  }
}

template <typename Synchronized, typename... Args>
auto wlock(Synchronized& synchronized, Args&&... args) {
  return detail::makeSynchronizedLocker(
      synchronized,
      [](auto& s, auto&&... a) {
        return s.wlock(std::forward<decltype(a)>(a)...);
      },
      [](auto& s) { return s.tryWLock(); },
      std::forward<Args>(args)...);
}
template <typename Synchronized, typename... Args>
auto rlock(Synchronized& synchronized, Args&&... args) {
  return detail::makeSynchronizedLocker(
      synchronized,
      [](auto& s, auto&&... a) {
        return s.rlock(std::forward<decltype(a)>(a)...);
      },
      [](auto& s) { return s.tryRLock(); },
      std::forward<Args>(args)...);
}
template <typename Synchronized, typename... Args>
auto ulock(Synchronized& synchronized, Args&&... args) {
  return detail::makeSynchronizedLocker(
      synchronized,
      [](auto& s, auto&&... a) {
        return s.ulock(std::forward<decltype(a)>(a)...);
      },
      [](auto& s) { return s.tryULock(); },
      std::forward<Args>(args)...);
}
template <typename Synchronized, typename... Args>
auto lock(Synchronized& synchronized, Args&&... args) {
  return detail::makeSynchronizedLocker(
      synchronized,
      [](auto& s, auto&&... a) {
        return s.lock(std::forward<decltype(a)>(a)...);
      },
      [](auto& s) { return s.tryLock(); },
      std::forward<Args>(args)...);
}

} // namespace detail

/**
 * This class temporarily unlocks a LockedPtr in a scoped manner.
 */
template <class SynchronizedType, class LockPolicy>
class ScopedUnlocker {
 public:
  explicit ScopedUnlocker(LockedPtr<SynchronizedType, LockPolicy>* p) noexcept
      : ptr_(p), parent_(p->parent()) {
    ptr_->releaseLock();
  }
  ScopedUnlocker(const ScopedUnlocker&) = delete;
  ScopedUnlocker& operator=(const ScopedUnlocker&) = delete;
  ScopedUnlocker(ScopedUnlocker&& other) noexcept
      : ptr_(std::exchange(other.ptr_, nullptr)),
        parent_(std::exchange(other.parent_, nullptr)) {}
  ScopedUnlocker& operator=(ScopedUnlocker&& other) = delete;

  ~ScopedUnlocker() noexcept(false) {
    if (ptr_) {
      ptr_->reacquireLock(parent_);
    }
  }

 private:
  LockedPtr<SynchronizedType, LockPolicy>* ptr_{nullptr};
  SynchronizedType* parent_{nullptr};
};

/**
 * A LockedPtr keeps a Synchronized<T> object locked for the duration of
 * LockedPtr's existence.
 *
 * It provides access the datum's members directly by using operator->() and
 * operator*().
 *
 * The LockPolicy parameter controls whether or not the lock is acquired in
 * exclusive or shared mode.
 */
template <class SynchronizedType, class LockPolicy>
class LockedPtr {
 private:
  constexpr static bool AllowsConcurrentAccess =
      LockPolicy::level != detail::SynchronizedMutexLevel::Unique;

  using CDataType = // the DataType with the appropriate const-qualification
      detail::SynchronizedDataType<SynchronizedType, AllowsConcurrentAccess>;

  template <typename LockPolicyOther>
  static constexpr bool kSameLevelAs =
      LockPolicy::level == LockPolicyOther::level;

  template <typename, typename>
  friend class LockedPtr;

  friend class ScopedUnlocker<SynchronizedType, LockPolicy>;

 public:
  using DataType = typename SynchronizedType::DataType;
  using MutexType = typename SynchronizedType::MutexType;
  using Synchronized = typename std::remove_const<SynchronizedType>::type;
  using LockType = detail::SynchronizedLockType<LockPolicy::level, MutexType>;

  /**
   * Creates an uninitialized LockedPtr.
   *
   * Dereferencing an uninitialized LockedPtr is not allowed.
   */
  LockedPtr() = default;

  /**
   * Takes a Synchronized<T> and locks it.
   */
  explicit LockedPtr(SynchronizedType* parent)
      : lock_{!parent ? LockType{} : doLock(parent->mutex_)} {}

  /**
   * Takes a Synchronized<T> and attempts to lock it, within the specified
   * timeout.
   *
   * Blocks until the lock is acquired or until the specified timeout expires.
   * If the timeout expired without acquiring the lock, the LockedPtr will be
   * null, and LockedPtr::isNull() will return true.
   */
  template <class Rep, class Period>
  LockedPtr(
      SynchronizedType* parent,
      const std::chrono::duration<Rep, Period>& timeout)
      : lock_{parent ? LockType{parent->mutex_, timeout} : LockType{}} {
    if (isNull()) {
      lock_ = {};
    }
  }

  /**
   * Move constructor.
   */
  LockedPtr(LockedPtr&& rhs) noexcept = default;
  template <typename Type = SynchronizedType>
    requires std::is_const_v<Type>
  /* implicit */ LockedPtr(LockedPtr<Synchronized, LockPolicy>&& rhs) noexcept
      : lock_{std::move(rhs.lock_)} {}
  template <typename LockPolicyType>
    requires kSameLevelAs<LockPolicyType>
  explicit LockedPtr(
      LockedPtr<SynchronizedType, LockPolicyType>&& other) noexcept
      : lock_{std::move(other.lock_)} {}
  template <typename Type = SynchronizedType, typename LockPolicyType>
    requires std::is_const_v<Type> && kSameLevelAs<LockPolicyType>
  explicit LockedPtr(LockedPtr<Synchronized, LockPolicyType>&& rhs) noexcept
      : lock_{std::move(rhs.lock_)} {}

  /**
   * Move assignment operator.
   */
  LockedPtr& operator=(LockedPtr&& rhs) noexcept = default;
  template <typename LockPolicyType>
    requires kSameLevelAs<LockPolicyType>
  LockedPtr& operator=(
      LockedPtr<SynchronizedType, LockPolicyType>&& other) noexcept {
    lock_ = std::move(other.lock_);
    return *this;
  }
  template <typename Type = SynchronizedType, typename LockPolicyType>
    requires std::is_const_v<Type> && kSameLevelAs<LockPolicyType>
  LockedPtr& operator=(
      LockedPtr<Synchronized, LockPolicyType>&& other) noexcept {
    lock_ = std::move(other.lock_);
    return *this;
  }

  /*
   * Copy constructor and assignment operator are deleted.
   */
  LockedPtr(const LockedPtr& rhs) = delete;
  LockedPtr& operator=(const LockedPtr& rhs) = delete;

  /**
   * Destructor releases.
   */
  ~LockedPtr() = default;

  /**
   * Access the underlying lock object.
   */
  LockType& as_lock() noexcept { return lock_; }
  LockType const& as_lock() const noexcept { return lock_; }

  /**
   * Check if this LockedPtr is uninitialized, or points to valid locked data.
   *
   * This method can be used to check if a timed-acquire operation succeeded.
   * If an acquire operation times out it will result in a null LockedPtr.
   *
   * A LockedPtr is always either null, or holds a lock to valid data.
   * Methods such as scopedUnlock() reset the LockedPtr to null for the
   * duration of the unlock.
   */
  bool isNull() const { return !lock_.owns_lock(); }

  /**
   * Explicit boolean conversion.
   *
   * Returns !isNull()
   */
  explicit operator bool() const { return lock_.owns_lock(); }

  /**
   * Access the locked data.
   *
   * This method should only be used if the LockedPtr is valid.
   */
  CDataType* operator->() const { return std::addressof(parent()->datum_); }

  /**
   * Access the locked data.
   *
   * This method should only be used if the LockedPtr is valid.
   */
  CDataType& operator*() const { return parent()->datum_; }

  void unlock() noexcept { lock_ = {}; }

  /**
   * Locks that allow concurrent access (shared, upgrade) force const
   * access with the standard accessors even if the Synchronized
   * object is non-const.
   *
   * In some cases non-const access can be needed, for example:
   *
   *   - Under an upgrade lock, to get references that will be mutated
   *     after upgrading to a write lock.
   *
   *   - Under an read lock, if some mutating operations on the data
   *     are thread safe (e.g. mutating the value in an associative
   *     container with reference stability).
   *
   * asNonConstUnsafe() returns a non-const reference to the data if
   * the parent Synchronized object was non-const at the point of lock
   * acquisition.
   */
  template <typename = void>
  DataType& asNonConstUnsafe() const {
    static_assert(
        AllowsConcurrentAccess && !std::is_const<SynchronizedType>::value,
        "asNonConstUnsafe() is only available on non-exclusive locks"
        " acquired in a non-const context");

    return parent()->datum_;
  }

  /**
   * Temporarily unlock the LockedPtr, and reset it to null.
   *
   * Returns an helper object that will re-lock and restore the LockedPtr when
   * the helper is destroyed.  The LockedPtr may not be dereferenced for as
   * long as this helper object exists.
   */
  ScopedUnlocker<SynchronizedType, LockPolicy> scopedUnlock() {
    return ScopedUnlocker<SynchronizedType, LockPolicy>(this);
  }

  /***************************************************************************
   * Upgrade lock methods.
   * These are disabled via SFINAE when the mutex is not an upgrade mutex.
   **************************************************************************/
  /**
   * Move the locked ptr from an upgrade state to an exclusive state.  The
   * current lock is left in a null state.
   */
  template <
      typename SyncType = SynchronizedType,
      decltype(void(std::declval<typename SyncType::MutexType&>()
                        .lock_upgrade()))* = nullptr>
  LockedPtr<SynchronizedType, detail::SynchronizedLockPolicyExclusive>
  moveFromUpgradeToWrite() {
    static_assert(std::is_same<SyncType, SynchronizedType>::value, "mismatch");
    return transition_to_unique_lock(lock_);
  }

  /**
   * Move the locked ptr from an exclusive state to an upgrade state.  The
   * current lock is left in a null state.
   */
  template <
      typename SyncType = SynchronizedType,
      decltype(void(std::declval<typename SyncType::MutexType&>()
                        .lock_upgrade()))* = nullptr>
  LockedPtr<SynchronizedType, detail::SynchronizedLockPolicyUpgrade>
  moveFromWriteToUpgrade() {
    static_assert(std::is_same<SyncType, SynchronizedType>::value, "mismatch");
    return transition_to_upgrade_lock(lock_);
  }

  /**
   * Move the locked ptr from an upgrade state to a shared state.  The
   * current lock is left in a null state.
   */
  template <
      typename SyncType = SynchronizedType,
      decltype(void(std::declval<typename SyncType::MutexType&>()
                        .lock_upgrade()))* = nullptr>
  LockedPtr<SynchronizedType, detail::SynchronizedLockPolicyShared>
  moveFromUpgradeToRead() {
    static_assert(std::is_same<SyncType, SynchronizedType>::value, "mismatch");
    return transition_to_shared_lock(lock_);
  }

  /**
   * Move the locked ptr from an exclusive state to a shared state.  The
   * current lock is left in a null state.
   */
  template <
      typename SyncType = SynchronizedType,
      decltype(void(std::declval<typename SyncType::MutexType&>()
                        .lock_shared()))* = nullptr>
  LockedPtr<SynchronizedType, detail::SynchronizedLockPolicyShared>
  moveFromWriteToRead() {
    static_assert(std::is_same<SyncType, SynchronizedType>::value, "mismatch");
    return transition_to_shared_lock(lock_);
  }

  SynchronizedType* parent() const {
    using simulacrum = typename SynchronizedType::Simulacrum;
    static_assert(sizeof(simulacrum) == sizeof(SynchronizedType), "mismatch");
    static_assert(alignof(simulacrum) == alignof(SynchronizedType), "mismatch");
    auto off = offsetof(simulacrum, mutex_);
    const auto raw = reinterpret_cast<char*>(lock_.mutex());
    return reinterpret_cast<SynchronizedType*>(raw - (raw ? off : 0));
  }

 private:
  /* implicit */ LockedPtr(LockType lock) noexcept : lock_{std::move(lock)} {}

  template <typename LP>
  static constexpr bool is_try =
      LP::method == detail::SynchronizedMutexMethod::TryLock;

  template <typename MT, typename LT = LockType, typename LP = LockPolicy>
    requires is_try<LP>
  FOLLY_ERASE static LT doLock(MT& mutex) {
    return LT{mutex, std::try_to_lock};
  }
  template <typename MT, typename LT = LockType, typename LP = LockPolicy>
    requires(!is_try<LP>)
  FOLLY_ERASE static LT doLock(MT& mutex) {
    return LT{mutex};
  }

  void releaseLock() noexcept {
    DCHECK(lock_.owns_lock());
    lock_ = {};
  }
  void reacquireLock(SynchronizedType* parent) {
    DCHECK(parent);
    DCHECK(!lock_.owns_lock());
    lock_ = doLock(parent->mutex_);
  }

  LockType lock_;
};

/**
 * Helper functions that should be passed to either a lock() or synchronized()
 * invocation, these return implementation defined structs that will be used
 * to lock the synchronized instance appropriately.
 *
 *    lock(wlock(one), rlock(two), wlock(three));
 *    synchronized([](auto one, two) { ... }, wlock(one), rlock(two));
 *
 * For example in the above rlock() produces an implementation defined read
 * locking helper instance and wlock() a write locking helper
 *
 * Subsequent arguments passed to these locking helpers, after the first, will
 * be passed by const-ref to the corresponding function on the synchronized
 * instance.  This means that if the function accepts these parameters by
 * value, they will be copied.  Note that it is not necessary that the primary
 * locking function will be invoked at all (for eg.  the implementation might
 * just invoke the try*Lock() method)
 *
 *    // Try to acquire the lock for one second
 *    synchronized([](auto) { ... }, wlock(one, 1s));
 *
 *    // The timed lock acquire might never actually be called, if it is not
 *    // needed by the underlying deadlock avoiding algorithm
 *    synchronized([](auto, auto) { ... }, rlock(one), wlock(two, 1s));
 *
 * Note that the arguments passed to to *lock() calls will be passed by
 * const-ref to the function invocation, as the implementation might use them
 * many times
 */
template <typename D, typename M, typename... Args>
auto wlock(Synchronized<D, M>& synchronized, Args&&... args) {
  return detail::wlock(synchronized, std::forward<Args>(args)...);
}
template <typename D, typename M, typename... Args>
auto wlock(const Synchronized<D, M>& synchronized, Args&&... args) {
  return detail::wlock(synchronized, std::forward<Args>(args)...);
}
template <typename Data, typename Mutex, typename... Args>
auto rlock(const Synchronized<Data, Mutex>& synchronized, Args&&... args) {
  return detail::rlock(synchronized, std::forward<Args>(args)...);
}
template <typename D, typename M, typename... Args>
auto ulock(Synchronized<D, M>& synchronized, Args&&... args) {
  return detail::ulock(synchronized, std::forward<Args>(args)...);
}
template <typename D, typename M, typename... Args>
auto lock(Synchronized<D, M>& synchronized, Args&&... args) {
  return detail::lock(synchronized, std::forward<Args>(args)...);
}
template <typename D, typename M, typename... Args>
auto lock(const Synchronized<D, M>& synchronized, Args&&... args) {
  return detail::lock(synchronized, std::forward<Args>(args)...);
}

/**
 * Acquire locks for multiple Synchronized<> objects, in a deadlock-safe
 * manner.
 *
 * Wrap the synchronized instances with the appropriate locking strategy by
 * using one of the four strategies - folly::lock (exclusive acquire for
 * exclusive only mutexes), folly::rlock (shared acquire for shareable
 * mutexes), folly::wlock (exclusive acquire for shareable mutexes) or
 * folly::ulock (upgrade acquire for upgrade mutexes) (see above)
 *
 * The locks will be acquired and the passed callable will be invoked with the
 * LockedPtr instances in the order that they were passed to the function
 */
template <typename Func, typename... SynchronizedLockers>
decltype(auto) synchronized(Func&& func, SynchronizedLockers&&... lockers) {
  return std::apply(
      std::forward<Func>(func),
      lock(std::forward<SynchronizedLockers>(lockers)...));
}

/**
 * Acquire locks on many lockables or synchronized instances in such a way
 * that the sequence of calls within the function does not cause deadlocks.
 *
 * This can often result in a performance boost as compared to simply
 * acquiring your locks in an ordered manner.  Even for very simple cases.
 * The algorithm tried to adjust to contention by blocking on the mutex it
 * thinks is the best fit, leaving all other mutexes open to be locked by
 * other threads.  See the benchmarks in folly/test/SynchronizedBenchmark.cpp
 * for more
 *
 * This works differently as compared to the locking algorithm in libstdc++
 * and is the recommended way to acquire mutexes in a generic order safe
 * manner.  Performance benchmarks show that this does better than the one in
 * libstdc++ even for the simple cases
 *
 * Usage is the same as std::lock() for arbitrary lockables
 *
 *    folly::lock(one, two, three);
 *
 * To make it work with folly::Synchronized you have to specify how you want
 * the locks to be acquired, use the folly::wlock(), folly::rlock(),
 * folly::ulock() and folly::lock() helpers defined below
 *
 *    auto [one, two] = lock(folly::wlock(a), folly::rlock(b));
 *
 * Note that you can/must avoid the folly:: namespace prefix on the lock()
 * function if you use the helpers, ADL lookup is done to find the lock function
 *
 * This will execute the deadlock avoidance algorithm and acquire a write lock
 * for a and a read lock for b
 */
template <typename LockableOne, typename LockableTwo, typename... Lockables>
void lock(LockableOne& one, LockableTwo& two, Lockables&... lockables) {
  auto locker = [](auto& lockable) {
    using Lockable = std::remove_reference_t<decltype(lockable)>;
    return detail::makeSynchronizedLocker(
        lockable,
        [](auto& l) { return std::unique_lock<Lockable>{l}; },
        [](auto& l) {
          return std::unique_lock<Lockable>{l, std::try_to_lock};
        });
  };
  auto locks = lock(locker(one), locker(two), locker(lockables)...);

  // release ownership of the locks from the RAII lock wrapper returned by the
  // function above
  for_each(locks, [&](auto& lock) { lock.release(); });
}

/**
 * Acquire locks for multiple Synchronized<T> objects, in a deadlock-safe
 * manner.
 *
 * The locks are acquired in order from lowest address to highest address.
 * (Note that this is not necessarily the same algorithm used by std::lock().)
 * For parameters that are const and support shared locks, a read lock is
 * acquired.  Otherwise an exclusive lock is acquired.
 *
 * use lock() with folly::wlock(), folly::rlock() and folly::ulock() for
 * arbitrary locking without causing a deadlock (as much as possible), with the
 * same effects as std::lock()
 */
template <class Sync1, class Sync2>
std::tuple<detail::LockedPtrType<Sync1>, detail::LockedPtrType<Sync2>>
acquireLocked(Sync1& l1, Sync2& l2) {
  if (static_cast<const void*>(&l1) < static_cast<const void*>(&l2)) {
    auto p1 = l1.contextualLock();
    auto p2 = l2.contextualLock();
    return std::make_tuple(std::move(p1), std::move(p2));
  } else {
    auto p2 = l2.contextualLock();
    auto p1 = l1.contextualLock();
    return std::make_tuple(std::move(p1), std::move(p2));
  }
}

/**
 * A version of acquireLocked() that returns a std::pair rather than a
 * std::tuple, which is easier to use in many places.
 */
template <class Sync1, class Sync2>
std::pair<detail::LockedPtrType<Sync1>, detail::LockedPtrType<Sync2>>
acquireLockedPair(Sync1& l1, Sync2& l2) {
  auto lockedPtrs = acquireLocked(l1, l2);
  return {
      std::move(std::get<0>(lockedPtrs)), std::move(std::get<1>(lockedPtrs))};
}

/************************************************************************
 * NOTE: All APIs below this line will be deprecated in upcoming diffs.
 ************************************************************************/

// Non-member swap primitive
template <class T, class M>
void swap(Synchronized<T, M>& lhs, Synchronized<T, M>& rhs) {
  lhs.swap(rhs);
}

/**
 * Disambiguate the name var by concatenating the line number of the original
 * point of expansion. This avoids shadowing warnings for nested
 * SYNCHRONIZEDs. The name is consistent if used multiple times within
 * another macro.
 * Only for internal use.
 */
#define SYNCHRONIZED_VAR(var) FB_CONCATENATE(SYNCHRONIZED_##var##_, __LINE__)

namespace detail {
struct [[deprecated("use explicit lock(), wlock(), or rlock() instead")]]
SYNCHRONIZED_macro_is_deprecated {};
} // namespace detail

/**
 * NOTE: This API is deprecated.  Use lock(), wlock(), rlock() or the withLock
 * functions instead.  In the future it will be marked with a deprecation
 * attribute to emit build-time warnings, and then it will be removed entirely.
 *
 * SYNCHRONIZED is the main facility that makes Synchronized<T>
 * helpful. It is a pseudo-statement that introduces a scope where the
 * object is locked. Inside that scope you get to access the unadorned
 * datum.
 *
 * Example:
 *
 * Synchronized<vector<int>> svector;
 * ...
 * SYNCHRONIZED (svector) { ... use svector as a vector<int> ... }
 * or
 * SYNCHRONIZED (v, svector) { ... use v as a vector<int> ... }
 *
 * Refer to folly/docs/Synchronized.md for a detailed explanation and more
 * examples.
 */
#define SYNCHRONIZED(...)                                                 \
  FOLLY_PUSH_WARNING                                                      \
  FOLLY_GNU_DISABLE_WARNING("-Wshadow")                                   \
  FOLLY_MSVC_DISABLE_WARNING(4189) /* initialized but unreferenced */     \
  FOLLY_MSVC_DISABLE_WARNING(4456) /* declaration hides local */          \
  FOLLY_MSVC_DISABLE_WARNING(4457) /* declaration hides parameter */      \
  FOLLY_MSVC_DISABLE_WARNING(4458) /* declaration hides member */         \
  FOLLY_MSVC_DISABLE_WARNING(4459) /* declaration hides global */         \
  FOLLY_GCC_DISABLE_NEW_SHADOW_WARNINGS                                   \
  if (bool SYNCHRONIZED_VAR(state) = false) {                             \
    (void)::folly::detail::SYNCHRONIZED_macro_is_deprecated{};            \
  } else                                                                  \
    for (auto SYNCHRONIZED_VAR(lockedPtr) =                               \
             (FB_VA_GLUE(FB_ARG_2_OR_1, (__VA_ARGS__))).contextualLock(); \
         !SYNCHRONIZED_VAR(state);                                        \
         SYNCHRONIZED_VAR(state) = true)                                  \
      for ([[maybe_unused]] auto& FB_VA_GLUE(FB_ARG_1, (__VA_ARGS__)) =   \
               *SYNCHRONIZED_VAR(lockedPtr).operator->();                 \
           !SYNCHRONIZED_VAR(state);                                      \
           SYNCHRONIZED_VAR(state) = true)                                \
    FOLLY_POP_WARNING

/**
 * NOTE: This API is deprecated.  Use lock(), wlock(), rlock() or the withLock
 * functions instead.  In the future it will be marked with a deprecation
 * attribute to emit build-time warnings, and then it will be removed entirely.
 *
 * Similar to SYNCHRONIZED, but only uses a read lock.
 */
#define SYNCHRONIZED_CONST(...)            \
  SYNCHRONIZED(                            \
      FB_VA_GLUE(FB_ARG_1, (__VA_ARGS__)), \
      std::as_const(FB_VA_GLUE(FB_ARG_2_OR_1, (__VA_ARGS__))))

/**
 * NOTE: This API is deprecated.  Use lock(), wlock(), rlock() or the withLock
 * functions instead.  In the future it will be marked with a deprecation
 * attribute to emit build-time warnings, and then it will be removed entirely.
 *
 * Synchronizes two Synchronized objects (they may encapsulate
 * different data). Synchronization is done in increasing address of
 * object order, so there is no deadlock risk.
 */
#define SYNCHRONIZED_DUAL(n1, e1, n2, e2)                                      \
  if (bool SYNCHRONIZED_VAR(state) = false) {                                  \
    (void)::folly::detail::SYNCHRONIZED_macro_is_deprecated{};                 \
  } else                                                                       \
    for (auto SYNCHRONIZED_VAR(ptrs) = acquireLockedPair(e1, e2);              \
         !SYNCHRONIZED_VAR(state);                                             \
         SYNCHRONIZED_VAR(state) = true)                                       \
      for (auto& n1 = *SYNCHRONIZED_VAR(ptrs).first; !SYNCHRONIZED_VAR(state); \
           SYNCHRONIZED_VAR(state) = true)                                     \
        for (auto& n2 = *SYNCHRONIZED_VAR(ptrs).second;                        \
             !SYNCHRONIZED_VAR(state);                                         \
             SYNCHRONIZED_VAR(state) = true)

} /* namespace folly */
