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

#include <string>
#include <string_view>
#include <vector>

#include <folly/CppAttributes.h>
#include <folly/Function.h>
#include <folly/Likely.h>
#include <folly/container/MapUtil.h>
#include <folly/settings/Types.h>
#include <folly/settings/detail/SettingsImpl.h>

namespace folly {
namespace settings {

class Snapshot;
namespace detail {

/**
 * @param TrivialPtr location of the small type storage.  Optimization
 *   for better inlining.
 */
template <class T, std::atomic<uint64_t>* TrivialPtr, typename Tag>
class SettingWrapper {
  using AccessCounter = typename SettingCore<T, Tag>::AccessCounter;

 public:
  using CallbackHandle = typename SettingCore<T, Tag>::CallbackHandle;

  /**
   * Returns the setting's current value. As an optimization, returns by value
   * for small types, and by const& for larger types. The returned reference is
   * only guaranteed to be valid until the next access by the current thread.
   *
   * UNSAFE:
   *   auto& value = *FOLLY_SETTING(project, my_string);
   *   *FOLLY_SETTING(project, my_string) // Access invalidates `value`
   *   useValue(value); // heap-use-after-free
   *
   * SAFE:
   *   auto& value = *FOLLY_SETTING(project, my_string);
   *   FOLLY_SETTING(project, my_string).set("abc"); // `value` is still valid
   *   useValue(value); // OK
   *
   * SAFE:
   *   Thread1:
   *     auto& value = *FOLLY_SETTING(project, my_string);
   *     useValue(value); // OK
   *   Thread2:
   *     auto& value = *FOLLY_SETTING(project, my_string);
   *     useValue(value); // OK
   *   Thread3:
   *    FOLLY_SETTING(project, my_string).set("abc");
   */
  std::conditional_t<IsSmallPOD<T>, T, const T&> operator*() const {
    AccessCounter::add(1);
    return core_.getWithHint(*TrivialPtr);
  }
  const T* operator->() const {
    AccessCounter::add(1);
    return &core_.getSlow().value;
  }

  /**
   * Returns the setting's current value as documented above by operator*().
   */
  std::conditional_t<IsSmallPOD<T>, T, const T&> value() const {
    return operator*();
  }
  /**
   * Same as value() but registers this setting as an observer dependency. If
   * this setting is used to compute an observer, subsequent setting updates
   * will trigger the recomputation of that observer.
   */
  std::conditional_t<IsSmallPOD<T>, T, const T&>
  valueRegisterObserverDependency() {
    if (observer_detail::ObserverManager::inManagerThread()) [[unlikely]] {
      registerObserverDependency();
    }
    return operator*();
  }

  /**
   * Returns the setting's value from snapshot. Following two forms are
   * equivalient:
   *   Snapshot snapshot;
   *   *snapshot(FOLLY_SETTING(proj, name)) ==
   *   FOLLY_SETTING(proj, name).value(snapshot);
   */
  std::conditional_t<IsSmallPOD<T>, T, const T&> value(
      const Snapshot& snapshot) const;

  /**
   * Returns an Observer<T> that's updated whenever this setting is updated.
   */
  const folly::observer::Observer<T>& observer() { return core_.observer(); }

  /**
   * Atomically updates the setting's current value. The next call to
   * operator*() will invalidate all references returned by previous calls to
   * operator*() on that thread.
   *
   * @param reason  Will be stored with the current value, useful for debugging.
   * @returns The SetResult indicating if the setting was successfully updated.
   * @throws std::runtime_error  If we can't convert t to string.
   */
  SetResult set(const T& t, std::string_view reason = "api") {
    return core_.set(t, reason);
  }

  /**
   * Adds a callback to be invoked any time the setting is updated. Callback
   * is not invoked for snapshot updates unless published.
   *
   * @param callback  void function that accepts a SettingsContents with value
   *        and reason, to be invoked on updates
   * @returns  a handle object which automatically removes the callback from
   *           processing once destroyd
   */
  CallbackHandle addCallback(
      typename SettingCore<T, Tag>::UpdateCallback callback) {
    return core_.addCallback(std::move(callback));
  }

  /**
   * Returns the default value this setting was constructed with.
   * NOTE: SettingsMetadata is type-agnostic, so it only stores the string
   * representation of the default value.  This method returns the
   * actual value that was passed on construction.
   */
  const T& defaultValue() const { return core_.defaultValue(); }

  /**
   * Returns the setting's current update reason.
   */
  std::string_view updateReason() const { return core_.getSlow().updateReason; }

  /**
   * Returns the number of times this setting has been accessed.
   */
  uint64_t accessCount() const { return AccessCounter::count(); }

  /**
   * Returns the setting's update reason in the snapshot.
   */
  std::string_view updateReason(const Snapshot& snapshot) const;

  /**
   * Returns the setting's name
   */
  std::string_view name() const { return core_.meta().name; }

  /**
   * Returns the setting's project
   */
  std::string_view project() const { return core_.meta().project; }

  /**
   * Returns the setting's description
   */
  std::string_view description() const { return core_.meta().description; }

  explicit SettingWrapper(SettingCore<T, Tag>& core) : core_(core) {}

 private:
  FOLLY_NOINLINE void registerObserverDependency() { observer().getSnapshot(); }

  SettingCore<T, Tag>& core_;
  friend class folly::settings::Snapshot;
};

template <
    typename T,
    std::atomic<uint64_t>* TrivialPtr,
    typename Tag,
    std::atomic<SettingCore<T, Tag>*>& CachedCore,
    SettingCore<T, Tag>& (*Func)()>
struct Accessor {
  /**
   * Optimization: fast-path on top of the Meyers singleton. We check the global
   * pointer which should only be initialized after the Meyers singleton. It's
   * ok for multiple calls to attempt to update the global pointer, as they
   * would be serialized on the Meyer's singleton initialization lock anyway.
   */
  FOLLY_ALWAYS_INLINE SettingWrapper<T, TrivialPtr, Tag> operator()() {
    auto* core = CachedCore.load(std::memory_order_acquire);
    if (!core) [[unlikely]] {
      core = &Func();
      CachedCore.store(core, std::memory_order_release);
    }
    return SettingWrapper<T, TrivialPtr, Tag>(*core);
  }
};
} // namespace detail

#if defined(_MSC_VER)
// MSVC does not support section attributes
#define FOLLY_SETTINGS_DETAIL_SECTION_ATTRIBUTE /* nothing */
#elif defined(__APPLE__)
// Mach-O: section attribute needs segment,section
#define FOLLY_SETTINGS_DETAIL_SECTION_ATTRIBUTE \
  gnu::section("__DATA,.folly.settings")
#else
// ELF: section attribute just needs section name
#define FOLLY_SETTINGS_DETAIL_SECTION_ATTRIBUTE \
  gnu::section(".folly.settings.cache")
#endif

/**
 * Registers a setting without defining an Accessor. This should rarely be used
 * directly and most use cases will want FOLLY_SETTING_DEFINE instead.
 */
#define FOLLY_SETTING_REGISTER(                                               \
    _project, _name, _Type, _def, _mut, _cli, _desc)                          \
  struct FOLLY_SETTINGS_TAG__##_project##_##_name {};                         \
  /* Fastpath optimization, see notes in FOLLY_SETTINGS_DEFINE_LOCAL_FUNC__.  \
     Aggregate all off these together in a single section for better TLB      \
     and cache locality. */                                                   \
  [[FOLLY_SETTINGS_DETAIL_SECTION_ATTRIBUTE]] [[maybe_unused]] ::std::atomic< \
      ::folly::settings::detail::                                             \
          SettingCore<_Type, FOLLY_SETTINGS_TAG__##_project##_##_name>*>      \
      FOLLY_SETTINGS_CACHE__##_project##_##_name;                             \
  /* Location for the small value cache (if _Type is small and trivial).      \
     Intentionally located right after the pointer cache above to take        \
     advantage of the prefetching */                                          \
  [[FOLLY_SETTINGS_DETAIL_SECTION_ATTRIBUTE]] ::std::atomic<::std::uint64_t>  \
      FOLLY_SETTINGS_TRIVIAL__##_project##_##_name;                           \
  /* Meyers singleton to avoid SIOF */                                        \
  FOLLY_NOINLINE ::folly::settings::detail::                                  \
      SettingCore<_Type, FOLLY_SETTINGS_TAG__##_project##_##_name>&           \
      FOLLY_SETTINGS_FUNC__##_project##_##_name() {                           \
    static ::folly::Indestructible<::folly::settings::detail::SettingCore<    \
        _Type,                                                                \
        FOLLY_SETTINGS_TAG__##_project##_##_name>>                            \
        setting(                                                              \
            ::folly::settings::SettingMetadata{                               \
                #_project,                                                    \
                #_name,                                                       \
                #_Type,                                                       \
                typeid(_Type),                                                \
                #_def,                                                        \
                _mut,                                                         \
                _cli,                                                         \
                _desc},                                                       \
            ::folly::type_t<_Type>{_def},                                     \
            FOLLY_SETTINGS_TRIVIAL__##_project##_##_name);                    \
    return *setting;                                                          \
  }                                                                           \
  /* Ensure the setting is registered even if not used in program */          \
  auto& FOLLY_SETTINGS_INIT__##_project##_##_name =                           \
      FOLLY_SETTINGS_FUNC__##_project##_##_name()
/**
 * Defines a setting.
 *
 * Settings are either mutable or immutable where mutable setting values can
 * change at runtime whereas immutable setting values can not be changed after
 * the setting project is frozen (see Immutables.h).
 *
 * FOLLY_SETTING_DEFINE() can only be placed in a single translation unit
 * and will be checked against accidental collisions.
 *
 * The setting API can be accessed via FOLLY_SETTING(project, name).<api_func>()
 * and is documented in the Setting class.
 *
 * All settings for a common namespace; (project, name) must be unique
 * for the whole program.  Collisions are verified at runtime on
 * program startup.
 *
 * @param _project  Project identifier, can only contain [a-zA-Z0-9]
 * @param _name  setting name within the project, can only contain [_a-zA-Z0-9].
 *   The string "<project>_<name>" must be unique for the whole program.
 * @param _Type  setting value type
 * @param _def   default value for the setting
 * @param _mut   mutability of the setting
 * @param _cli   determines if a setting can be set through the command line
 * @param _desc  setting documentation
 */
#define FOLLY_SETTING_DEFINE(_project, _name, _Type, _def, _mut, _cli, _desc) \
  FOLLY_SETTING_REGISTER(_project, _name, _Type, _def, _mut, _cli, _desc);    \
  ::folly::settings::detail::Accessor<                                        \
      _Type,                                                                  \
      &FOLLY_SETTINGS_TRIVIAL__##_project##_##_name,                          \
      FOLLY_SETTINGS_TAG__##_project##_##_name,                               \
      FOLLY_SETTINGS_CACHE__##_project##_##_name,                             \
      FOLLY_SETTINGS_FUNC__##_project##_##_name>                              \
      FOLLY_SETTINGS_ACCESSOR__##_project##_##_name

/**
 * Declares a setting that's defined elsewhere.
 */
#define FOLLY_SETTING_DECLARE(_project, _name, _Type)               \
  struct FOLLY_SETTINGS_TAG__##_project##_##_name;                  \
  extern ::std::atomic<::folly::settings::detail::SettingCore<      \
      _Type,                                                        \
      FOLLY_SETTINGS_TAG__##_project##_##_name>*>                   \
      FOLLY_SETTINGS_CACHE__##_project##_##_name;                   \
  extern ::std::atomic<::std::uint64_t>                             \
      FOLLY_SETTINGS_TRIVIAL__##_project##_##_name;                 \
  ::folly::settings::detail::                                       \
      SettingCore<_Type, FOLLY_SETTINGS_TAG__##_project##_##_name>& \
      FOLLY_SETTINGS_FUNC__##_project##_##_name();                  \
  extern ::folly::settings::detail::Accessor<                       \
      _Type,                                                        \
      &FOLLY_SETTINGS_TRIVIAL__##_project##_##_name,                \
      FOLLY_SETTINGS_TAG__##_project##_##_name,                     \
      FOLLY_SETTINGS_CACHE__##_project##_##_name,                   \
      FOLLY_SETTINGS_FUNC__##_project##_##_name>                    \
      FOLLY_SETTINGS_ACCESSOR__##_project##_##_name

/**
 * Accesses a defined setting.
 * Rationale for the macro:
 *  1) Searchability, all settings access is done via FOLLY_SETTING(...)
 *  2) Prevents omitting trailing () by accident, which could
 *     lead to bugs like `auto value = *FOLLY_SETTING_project_name;`,
 *     which compiles but dereferences the function pointer instead of
 *     the setting itself.
 */
#define FOLLY_SETTING(_project, _name) \
  FOLLY_SETTINGS_ACCESSOR__##_project##_##_name()

/**
 * @return If the setting exists, returns the current settings metadata.
 *         Empty Optional otherwise.
 */
Optional<SettingMetadata> getSettingsMeta(std::string_view settingName);

/**
 * @return SettingMetadata for all registered settings in the process.
 */
std::vector<SettingMetadata> getAllSettingsMeta();

/**
 * @return If the setting exists and has type T, returns the default value
 * defined by FOLLY_SETTING_DEFINE.
 */
template <typename T>
const T* FOLLY_NULLABLE getDefaultValue(const std::string& settingName) {
  auto* eptr = get_default(*detail::settingsMap().rlock(), settingName);
  auto* tptr = dynamic_cast<detail::TypedSettingCore<T>*>(eptr);
  return !tptr ? nullptr : &tptr->defaultValue();
}

namespace detail {

/**
 * Like SettingWrapper, but checks against any values saved/updated in a
 * snapshot.
 */
template <class T, typename Tag>
class SnapshotSettingWrapper {
 public:
  /**
   * The references are only valid for the duration of the snapshot's
   * lifetime or until the setting has been updated in the snapshot,
   * whichever happens earlier.
   */
  const T& operator*() const;
  const T* operator->() const { return &operator*(); }

  /**
   * Update the setting in the snapshot, the effects are not visible
   * in this snapshot.
   * @returns The SetResult indicating if the setting was successfully updated.
   */
  SetResult set(const T& t, std::string_view reason = "api") {
    return core_.set(t, reason, &snapshot_);
  }

 private:
  Snapshot& snapshot_;
  SettingCore<T, Tag>& core_;
  friend class folly::settings::Snapshot;

  SnapshotSettingWrapper(Snapshot& snapshot, SettingCore<T, Tag>& core)
      : snapshot_(snapshot), core_(core) {}
};

} // namespace detail

/**
 * Captures the current state of all setting values and allows
 * updating multiple settings at once, with verification and rollback.
 *
 * A single snapshot cannot be used concurrently from different
 * threads.  Multiple concurrent snapshots are safe. Passing a single
 * snapshot from one thread to another is safe as long as the user
 * properly synchronizes the handoff.
 *
 * Example usage:
 *
 *   folly::settings::Snapshot snapshot;
 *   // FOLLY_SETTING(project, name) refers to the globally visible value
 *   // snapshot(FOLLY_SETTING(project, name)) refers to the value saved in the
 *   //  snapshot
 *   FOLLY_SETTING(project, name).set(new_value);
 *   assert(*FOLLY_SETTING(project, name) == new_value);
 *   assert(*snapshot(FOLLY_SETTING(project, name)) == old_value);
 *
 *   snapshot(FOLLY_SETTING(project, name)).set(new_snapshot_value);
 *   assert(*FOLLY_SETTING(project, name) == new_value);
 *   assert(*snapshot(FOLLY_SETTING(project, name)) == new_snapshot_value);
 *
 *   // At this point we can discard the snapshot and forget new_snapshot_value,
 *   // or choose to publish:
 *   snapshot.publish();
 *   assert(*FOLLY_SETTING(project, name) == new_snapshot_value);
 */
class Snapshot final : public detail::SnapshotBase {
 public:
  /**
   * Wraps a global FOLLY_SETTING(a, b) and returns a snapshot-local wrapper.
   */
  template <class T, std::atomic<uint64_t>* P, typename Tag>
  detail::SnapshotSettingWrapper<T, Tag> operator()(
      detail::SettingWrapper<T, P, Tag>&& setting) {
    return detail::SnapshotSettingWrapper<T, Tag>(*this, setting.core_);
  }

  /**
   * Returns a snapshot of all current setting values.
   * Global settings changes will not be visible in the snapshot, and vice
   * versa.
   */
  Snapshot() = default;

  /**
   * Apply all settings updates from this snapshot to the global state
   * unconditionally.
   */
  void publish() override;

  /**
   * Look up a setting by name, and update the value from a string
   * representation.
   *
   * @returns The SetResult indicating if the setting was successfully updated.
   * @throws std::runtime_error  If there's a conversion error.
   */
  SetResult setFromString(
      std::string_view settingName,
      std::string_view newValue,
      std::string_view reason) override;

  /**
   * Same as setFromString but will set frozen immutables in this snapshot.
   * However, it will still not publish them. This is mainly useful for setting
   * change dry-runs.
   */
  SetResult forceSetFromString(
      std::string_view settingName,
      std::string_view newValue,
      std::string_view reason) override;

  /**
   * @return If the setting exists, the current setting information.
   *         Empty Optional otherwise.
   */
  Optional<SettingsInfo> getAsString(
      std::string_view settingName) const override;

  /**
   * Reset the value of the setting identified by name to its default value.
   * The reason will be set to "default".
   *
   * @returns The SetResult indicating if the setting was successfully reset.
   */
  SetResult resetToDefault(std::string_view settingName) override;

  /**
   * Same as resetToDefault but will reset frozen immutables in this snapshot.
   * However, it will still not publish them. This is mainly useful for setting
   * change dry-runs.
   */
  SetResult forceResetToDefault(std::string_view settingName) override;

  /**
   * Iterates over all known settings and calls func(visitorInfo) for each.
   */
  void forEachSetting(
      FunctionRef<void(const SettingVisitorInfo&)> func) const override;

 private:
  template <typename T, typename Tag>
  friend class detail::SnapshotSettingWrapper;

  template <typename T, std::atomic<uint64_t>* TrivialPtr, typename Tag>
  friend class detail::SettingWrapper;
};

namespace detail {
template <class T, typename Tag>
inline const T& SnapshotSettingWrapper<T, Tag>::operator*() const {
  return snapshot_.get(core_).value;
}

template <class T, std::atomic<uint64_t>* TrivialPtr, typename Tag>
inline std::conditional_t<IsSmallPOD<T>, T, const T&>
SettingWrapper<T, TrivialPtr, Tag>::value(const Snapshot& snapshot) const {
  return snapshot.get(core_).value;
}

template <class T, std::atomic<uint64_t>* TrivialPtr, typename Tag>
std::string_view SettingWrapper<T, TrivialPtr, Tag>::updateReason(
    const Snapshot& snapshot) const {
  return snapshot.get(core_).updateReason;
}
} // namespace detail

} // namespace settings
} // namespace folly
