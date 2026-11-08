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

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

#include <fmt/format.h>

#include <folly/Conv.h>
#include <folly/Function.h>
#include <folly/Optional.h>
#include <folly/Portability.h>
#include <folly/SharedMutex.h>
#include <folly/ThreadLocal.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/concurrency/SingletonRelaxedCounter.h>
#include <folly/container/F14Set.h>
#include <folly/lang/Aligned.h>
#include <folly/observer/Observer.h>
#include <folly/observer/SimpleObservable.h>
#include <folly/settings/Immutables.h>
#include <folly/settings/Types.h>
#include <folly/synchronization/DelayedInit.h>
#include <folly/synchronization/RelaxedAtomic.h>

namespace folly {
namespace settings {
namespace detail {

/**
 * Can we store T in a global atomic?
 */
template <class T>
constexpr bool IsSmallPOD =
    std::is_trivial_v<T> && sizeof(T) <= sizeof(uint64_t);

template <class T>
struct SettingContents {
  std::string updateReason;
  T value;

  template <class... Args>
  SettingContents(std::string _reason, Args&&... args)
      : updateReason(std::move(_reason)), value(std::forward<Args>(args)...) {}
};

class SnapshotBase;

class SettingCoreBase {
 public:
  using Key = intptr_t;
  using Version = uint64_t;

  virtual SetResult setFromString(
      std::string_view newValue,
      std::string_view reason,
      SnapshotBase* snapshot) = 0;
  virtual void forceSetFromString(
      std::string_view newValue,
      std::string_view reason,
      SnapshotBase* snapshot) = 0;
  virtual std::string_view getUpdateReason(
      const SnapshotBase* snapshot) const = 0;
  virtual std::pair<std::string, std::string> getAsString(
      const SnapshotBase* snapshot) const = 0;
  virtual SetResult resetToDefault(SnapshotBase* snapshot) = 0;
  virtual void forceResetToDefault(SnapshotBase* snapshot) = 0;
  virtual const SettingMetadata& meta() const = 0;
  virtual uint64_t accessCount() const = 0;
  virtual bool hasHadCallbacks() const = 0;
  virtual ~SettingCoreBase() {}

  /**
   * Hashable key uniquely identifying this setting in this process
   */
  Key getKey() const { return reinterpret_cast<Key>(this); }
};

void registerSetting(SettingCoreBase& core);

using SettingsMap = std::map<std::string, SettingCoreBase*>;
Synchronized<SettingsMap>& settingsMap();

/**
 * Returns the monotonically increasing unique positive version.
 */
SettingCoreBase::Version nextGlobalVersion();

template <class T, typename Tag>
class SettingCore;

/**
 * Type erasure for setting values
 */
class BoxedValue {
 public:
  BoxedValue() = default;

  /**
   * Stores a value that can be retrieved later
   */
  template <class T>
  explicit BoxedValue(const SettingContents<T>& value)
      : value_(std::make_shared<SettingContents<T>>(value)) {}

  /**
   * Stores a value that can be both retrieved later and optionally
   * applied globally
   */
  template <class T, typename Tag>
  BoxedValue(const T& value, std::string_view reason, SettingCore<T, Tag>& core)
      : value_(
            std::make_shared<SettingContents<T>>(std::string(reason), value)),
        core_{&core},
        publish_{doPublish<T, Tag>} {}

  /**
   * Returns the reference to the stored value
   */
  template <class T>
  const SettingContents<T>& unbox() const {
    return BoxedValue::unboxImpl<T>(value_.get());
  }

  /**
   * Applies the stored value globally
   */
  void publish(const FrozenSettingProjects& frozenProjects) {
    if (publish_) {
      publish_(*this, frozenProjects);
    }
  }

 private:
  using PublishFun = void(BoxedValue&, const FrozenSettingProjects&);

  template <typename T, typename Tag>
  static void doPublish(
      BoxedValue& boxed, const FrozenSettingProjects& frozenProjects) {
    auto& core = *static_cast<SettingCore<T, Tag>*>(boxed.core_);
    if (core.meta().mutability == Mutability::Immutable &&
        frozenProjects.contains(core.meta().project)) {
      return;
    }
    auto& contents = BoxedValue::unboxImpl<T>(boxed.value_.get());
    core.setImpl(
        contents.value, contents.updateReason, /* snapshot = */ nullptr);
  }

  template <class T>
  static const SettingContents<T>& unboxImpl(void* value) {
    return *static_cast<const SettingContents<T>*>(value);
  }

  std::shared_ptr<void> value_;
  SettingCoreBase* core_{};
  PublishFun* publish_{};
};

/**
 * If there are any outstanding snapshots that care about this
 * value that's about to be updated, save it to extend its lifetime
 */
void saveValueForOutstandingSnapshots(
    SettingCoreBase::Key settingKey,
    SettingCoreBase::Version version,
    const BoxedValue& value);

/**
 * @returns a pointer to a saved value at or before the given version
 */
const BoxedValue* getSavedValue(
    SettingCoreBase::Key key, SettingCoreBase::Version at);

template <typename T>
class TypedSettingCore : public SettingCoreBase {
 public:
  using Contents = SettingContents<T>;
  SetResult setFromString(
      std::string_view newValue,
      std::string_view reason,
      SnapshotBase* snapshot) override {
    if (isFrozenImmutable()) {
      // Return the error before calling convertOrConstruct in case it throws.
      return makeUnexpected(SetErrorCode::FrozenImmutable);
    }
    forceSetFromString(newValue, reason, snapshot);
    return unit;
  }

  void forceSetFromString(
      std::string_view newValue,
      std::string_view reason,
      SnapshotBase* snapshot) override {
    setImpl(convertOrConstruct(newValue), reason, snapshot);
  }

  std::pair<std::string, std::string> getAsString(
      const SnapshotBase* snapshot) const override;

  std::string_view getUpdateReason(const SnapshotBase* snapshot) const override;

  SetResult resetToDefault(SnapshotBase* snapshot) override {
    if (isFrozenImmutable()) {
      // Return the error before calling convertOrConstruct in case it throws.
      return makeUnexpected(SetErrorCode::FrozenImmutable);
    }
    forceResetToDefault(snapshot);
    return folly::unit;
  }

  void forceResetToDefault(SnapshotBase* snapshot) override {
    setImpl(defaultValue_, "default", snapshot);
  }

  const SettingMetadata& meta() const override { return meta_; }

  /**
   * @param trivialStorage must refer to the same location
   *   as the internal trivialStorage_.  This hint will
   *   generate better inlined code since the address is known
   *   at compile time at the callsite.
   */
  std::conditional_t<IsSmallPOD<T>, T, const T&> getWithHint(
      std::atomic<uint64_t>& trivialStorage) const {
    if constexpr (IsSmallPOD<T>) {
      uint64_t v = trivialStorage.load();
      T t;
      std::memcpy(&t, &v, sizeof(T));
      return t;
    } else {
      return const_cast<TypedSettingCore*>(this)->tlValue()->value;
    }
  }
  const SettingContents<T>& getSlow() const { return *tlValue(); }

  SetResult set(
      const T& t, std::string_view reason, SnapshotBase* snapshot = nullptr) {
    if (isFrozenImmutable()) {
      return makeUnexpected(SetErrorCode::FrozenImmutable);
    }
    setImpl(t, reason, snapshot);
    return unit;
  }

  const T& defaultValue() const { return defaultValue_; }

 private:
  SettingMetadata meta_;
  const T defaultValue_;

 protected:
  TypedSettingCore(
      SettingMetadata meta,
      T defaultValue,
      std::atomic<uint64_t>& trivialStorage)
      : meta_(std::move(meta)),
        defaultValue_(std::move(defaultValue)),
        trivialStorage_(trivialStorage) {}

  mutable SharedMutex globalLock_;

  // Tracks the number of calls to sanitizeModeInvalidateThreadLocalReferences.
  // This field is stored after globalLock_ to avoid increasing layout size.
  mutable relaxed_atomic<size_t> numInvalidateThreadLocalReferencesCalls_{0};

  // Limits the number of calls to sanitizeModeInvalidateThreadLocalReferences
  // to reduce the performance impact of these checks for ASAN builds.
  static constexpr const size_t kMaxNumInvalidatesPerSettingInSanitizeMode = 10;

  // Only mutable for use in sanitizeModeMaybeInvalidateThreadLocalReferences.
  mutable std::shared_ptr<Contents> globalValue_;

  std::atomic<uint64_t>& trivialStorage_;

  /* Thread local versions start at 0, this will force a read on first access.
   */
  cacheline_aligned<std::atomic<Version>> settingVersion_{std::in_place, 1};

 private:
  using LocalValue = std::pair<Version, std::shared_ptr<Contents>>;
  struct LocalValueTLP : cacheline_aligned<LocalValue> {
    LocalValueTLP() noexcept
        : cacheline_aligned<LocalValue>(std::in_place, 0, nullptr) {}
  };

  mutable ThreadLocalPtr<LocalValueTLP> localValue_;

  FOLLY_ALWAYS_INLINE LocalValue& getLocalValue() const {
    auto const ptr = localValue_.get();
    return FOLLY_LIKELY(!!ptr) ? **ptr : getLocalValueSlow();
  }
  FOLLY_NOINLINE LocalValue& getLocalValueSlow() const {
    auto const ptr = new LocalValueTLP();
    localValue_.reset(ptr);
    return **ptr;
  }

  void sanitizeModeInvalidateThreadLocalReferences(
      std::shared_ptr<Contents>& value) const {
    std::unique_lock lg(globalLock_);
    globalValue_ = std::make_shared<Contents>(*globalValue_);
    value = globalValue_;
  }

  FOLLY_ALWAYS_INLINE void sanitizeModeMaybeInvalidateThreadLocalReferences(
      std::shared_ptr<Contents>& value) const {
    // If the setting is a non-trivial type such that operator* returns a
    // reference and we're running under ASAN, we add spurious no-op setting
    // updates that invalidates all references stored by the current thread.
    // This makes reference stability problems much more likely to be caught
    // before they're triggered in production by something like a config change.
    if constexpr (kIsLibrarySanitizeAddress && !detail::IsSmallPOD<T>) {
      if (numInvalidateThreadLocalReferencesCalls_.load() <
              kMaxNumInvalidatesPerSettingInSanitizeMode &&
          numInvalidateThreadLocalReferencesCalls_.fetch_add(1) <
              kMaxNumInvalidatesPerSettingInSanitizeMode) {
        sanitizeModeInvalidateThreadLocalReferences(value);
      }
    }
  }

  FOLLY_ALWAYS_INLINE const std::shared_ptr<Contents>& tlValue() const {
    auto& value = getLocalValue();
    if (value.first == *settingVersion_) [[likely]] {
      sanitizeModeMaybeInvalidateThreadLocalReferences(value.second);
      return value.second;
    }
    return tlValueSlow();
  }
  FOLLY_NOINLINE const std::shared_ptr<Contents>& tlValueSlow() const {
    auto& value = getLocalValue();
    while (value.first < *settingVersion_) {
      /* If this destroys the old value, do it without holding the lock */
      value.second.reset();
      std::shared_lock lg(globalLock_);
      value.first = *settingVersion_;
      value.second = globalValue_;
    }
    return value.second;
  }

  virtual void setImpl(
      const T& t, std::string_view reason, SnapshotBase* snapshot) = 0;

  bool isFrozenImmutable() const {
    switch (meta_.mutability) {
      case Mutability::Mutable:
        return false;
      case Mutability::Immutable:
        return immutablesFrozen(meta_.project);
    }
  }

  T convertOrConstruct(std::string_view newValue) {
    if constexpr (std::is_constructible_v<T, std::string_view>) {
      return T(newValue);
    } else {
      SettingValueAndMetadata from(newValue, meta_);
      return to<T>(from);
    }
  }
};

class SnapshotBase {
 public:
  /**
   * Type that encapsulates the current pair of (to<string>(value), reason)
   */
  using SettingsInfo = std::pair<std::string, std::string>;

  struct SettingVisitorInfo {
    SettingVisitorInfo(
        const std::string& fullName,
        const SettingCoreBase& core,
        const SnapshotBase& snapshot)
        : fullName_(fullName), core_(core), snapshot_(snapshot) {}

    const SettingMetadata& meta() const { return core_.meta(); }
    std::string_view updateReason() const;
    std::pair<std::string, std::string> valueAndReason() const;
    const std::string& fullName() const { return fullName_; }
    uint64_t accessCount() const { return core_.accessCount(); }
    bool hasHadCallbacks() const { return core_.hasHadCallbacks(); }

   private:
    const std::string& fullName_;
    const SettingCoreBase& core_;
    const SnapshotBase& snapshot_;
  };

  /**
   * Apply all settings updates from this snapshot to the global state
   * unconditionally.
   */
  virtual void publish() = 0;

  /**
   * Look up a setting by name, and update the value from a string
   * representation.
   *
   * @returns The SetResult indicating if the setting was successfully updated.
   * @throws std::runtime_error  If there's a conversion error.
   */
  virtual SetResult setFromString(
      std::string_view settingName,
      std::string_view newValue,
      std::string_view reason) = 0;

  /**
   * Same as setFromString but will set frozen immutables in this snapshot.
   * However, it will still not publish them. This is mainly useful for setting
   * change dry-runs.
   */
  virtual SetResult forceSetFromString(
      std::string_view settingName,
      std::string_view newValue,
      std::string_view reason) = 0;

  /**
   * @return If the setting exists, the current setting information.
   *         Empty Optional otherwise.
   */
  virtual Optional<SettingsInfo> getAsString(
      std::string_view settingName) const = 0;

  /**
   * Reset the value of the setting identified by name to its default value.
   * The reason will be set to "default".
   *
   * @returns The SetResult indicating if the setting was successfully reset.
   */
  virtual SetResult resetToDefault(std::string_view settingName) = 0;

  /**
   * Same as resetToDefault but will reset frozen immutables in this snapshot.
   * However, it will still not publish them. This is mainly useful for setting
   * change dry-runs.
   */
  virtual SetResult forceResetToDefault(std::string_view settingName) = 0;

  /**
   * Iterates over all known settings and calls func(visitorInfo) for each.
   */
  virtual void forEachSetting(
      FunctionRef<void(const SettingVisitorInfo&)> func) const = 0;

  virtual ~SnapshotBase();

 protected:
  SettingCoreBase::Version at_;
  std::unordered_map<SettingCoreBase::Key, BoxedValue> snapshotValues_;

  template <typename T>
  friend class TypedSettingCore;
  template <typename T, typename Tag>
  friend class SettingCore;

  SnapshotBase();

  SnapshotBase(const SnapshotBase&) = delete;
  SnapshotBase& operator=(const SnapshotBase&) = delete;
  SnapshotBase(SnapshotBase&&) = delete;
  SnapshotBase& operator=(SnapshotBase&&) = delete;

  template <class T>
  const SettingContents<T>& get(const TypedSettingCore<T>& core) const {
    auto it = snapshotValues_.find(core.getKey());
    if (it != snapshotValues_.end()) {
      return it->second.template unbox<T>();
    }
    auto savedValue = getSavedValue(core.getKey(), at_);
    if (savedValue) {
      return savedValue->template unbox<T>();
    }
    return core.getSlow();
  }

  template <class T, typename Tag>
  void set(SettingCore<T, Tag>& core, const T& t, std::string_view reason) {
    snapshotValues_[core.getKey()] = BoxedValue(t, reason, core);
  }
};

template <typename T>
std::pair<std::string, std::string> TypedSettingCore<T>::getAsString(
    const SnapshotBase* snapshot) const {
  auto& contents = snapshot ? snapshot->get(*this) : getSlow();
  return std::make_pair(
      folly::to<std::string>(contents.value), contents.updateReason);
}
template <typename T>
std::string_view TypedSettingCore<T>::getUpdateReason(
    const SnapshotBase* snapshot) const {
  auto& contents = snapshot ? snapshot->get(*this) : getSlow();
  return contents.updateReason;
}

template <typename F>
struct NamedObserverCreator {
  NamedObserverCreator(std::string name, F&& creator)
      : name_(std::move(name)), creator_(std::forward<F>(creator)) {}

  auto operator()() { return creator_(); }

  const std::string& getName() const { return name_; }

 private:
  std::string name_;
  F creator_;
};

template <class T, typename Tag>
class SettingCore : public TypedSettingCore<T> {
 public:
  using Contents = typename TypedSettingCore<T>::Contents;
  using AccessCounter = SingletonRelaxedCounter<uint64_t, Tag>;

  uint64_t accessCount() const override { return AccessCounter::count(); }

  bool hasHadCallbacks() const override { return hasHadCallbacks_.load(); }

  using UpdateCallback = Function<void(const Contents&)>;
  class CallbackHandle {
   public:
    CallbackHandle(
        std::shared_ptr<UpdateCallback> callback, SettingCore<T, Tag>& setting)
        : callback_(std::move(callback)), setting_(setting) {}
    ~CallbackHandle() {
      if (callback_) {
        std::unique_lock lg(setting_.globalLock_);
        setting_.callbacks_.erase(callback_);
      }
    }
    CallbackHandle(const CallbackHandle&) = delete;
    CallbackHandle& operator=(const CallbackHandle&) = delete;
    CallbackHandle(CallbackHandle&&) = default;
    CallbackHandle& operator=(CallbackHandle&&) = default;

   private:
    std::shared_ptr<UpdateCallback> callback_;
    SettingCore<T, Tag>& setting_;
  };
  CallbackHandle addCallback(UpdateCallback callback) {
    hasHadCallbacks_.store(true);
    auto callbackPtr = copy_to_shared_ptr(std::move(callback));

    auto copiedPtr = callbackPtr;
    {
      std::unique_lock lg(this->globalLock_);
      callbacks_.emplace(std::move(copiedPtr));
    }
    return CallbackHandle(std::move(callbackPtr), *this);
  }

  /**
   * Returns an Observer<T> that's updated whenever this setting is updated.
   */
  const observer::Observer<T>& observer() {
    return observer_.try_emplace_with([this]() { return createObserver(); });
  }

  SettingCore(
      SettingMetadata meta,
      T defaultValue,
      std::atomic<uint64_t>& trivialStorage)
      : TypedSettingCore<T>(
            std::move(meta), std::move(defaultValue), trivialStorage) {
    this->forceResetToDefault(/* snapshot */ nullptr);
    registerSetting(*this);
  }

 private:
  friend class BoxedValue;

  F14FastSet<std::shared_ptr<UpdateCallback>> callbacks_;
  std::atomic<bool> hasHadCallbacks_{false};
  DelayedInit<observer::Observer<T>> observer_;

  void setImpl(
      const T& t, std::string_view reason, SnapshotBase* snapshot) override {
    /* Check that we can still display it (will throw otherwise) */
    folly::to<std::string>(t);

    if (snapshot) {
      snapshot->set(*this, t, reason);
      return;
    }

    {
      std::unique_lock lg(this->globalLock_);

      if (this->globalValue_) {
        saveValueForOutstandingSnapshots(
            this->getKey(),
            *this->settingVersion_,
            BoxedValue(*this->globalValue_));
      }
      this->globalValue_ = std::make_shared<Contents>(std::string(reason), t);
      if constexpr (IsSmallPOD<T>) {
        uint64_t v = 0;
        std::memcpy(&v, &t, sizeof(T));
        this->trivialStorage_.store(v);
      }
      *this->settingVersion_ = nextGlobalVersion();
    }
    if constexpr (kIsLibrarySanitizeAddress && !detail::IsSmallPOD<T>) {
      this->numInvalidateThreadLocalReferencesCalls_.store(0);
    }
    invokeCallbacks(Contents(std::string(reason), t));
  }

  void invokeCallbacks(const Contents& contents) {
    auto callbacksSnapshot = std::invoke([&] {
      std::shared_lock lg(this->globalLock_);
      // invoking arbitrary user code under the lock is dangerous
      return std::vector<std::shared_ptr<UpdateCallback>>(
          callbacks_.begin(), callbacks_.end());
    });

    for (auto& callbackPtr : callbacksSnapshot) {
      auto& callback = *callbackPtr;
      callback(contents);
    }
  }
  /**
   * Creates a folly::observer::Observer<T> for this setting that's updated
   * whenever this setting is updated.
   */
  observer::Observer<T> createObserver() {
    // Make observable a unique_ptr so it can be moved and captured in the
    // setting update callback
    auto setting = this->getWithHint(this->trivialStorage_);
    auto observable = std::make_unique<observer::SimpleObservable<T>>(setting);
    auto observer = observable->getObserver();
    auto callbackHandle = addCallback(
        [observable = std::move(observable)](const auto& newContents) {
          observable->setValue(newContents.value);
        });
    // Create a wrapped observer to capture the callback handle and keep it
    // alive as long as the observer is alive
    auto& meta = this->meta();
    NamedObserverCreator creator(
        fmt::format("FOLLY_SETTING_{}_{}", meta.project, meta.name),
        [callbackHandle = std::move(callbackHandle),
         observer = std::move(observer)]() { return **observer; });
    if constexpr (IsEqualityComparable<T>::value) {
      return observer::makeValueObserver(std::move(creator));
    } else {
      return observer::makeObserver(std::move(creator));
    }
  }
};

} // namespace detail
} // namespace settings
} // namespace folly
