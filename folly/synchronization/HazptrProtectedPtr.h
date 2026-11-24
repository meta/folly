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
#include <type_traits>
#include <utility>

#include <folly/synchronization/HazptrHolder.h>

namespace folly {

/// hazptr_protected_ptr
///
/// Smart-pointer-like view of a hazard-pointer-protected object.
///
/// A hazptr_protected_ptr pins the reclamation of the object it views for
/// exactly as long as the guard object lives, without any refcount RMWs:
/// readers touch only thread-local hazard state plus a plain load. Typical
/// use is a read-mostly published pointer with rare writers:
///
///   std::atomic<Config*> config_;
///   ...
///   hazptr_protected_ptr<Config> cfg(config_);
///   if (cfg) {
///     use(*cfg);
///   }
///   // protection released here
///
/// Protection is not ownership. The guard keeps a retired object from being
/// reclaimed, but it does not keep it alive past the teardown of whatever
/// owns it: protections must have lifetimes bounded by the lifetime of the
/// domain or cohort the object retires to, and the use case is responsible
/// for obeying that requirement. In practice the guard must not outlive the
/// owner of the protected state (for example, keep the owning object alive
/// with a reference count or a keep-alive while any guard may exist).
/// Destroying the owner while a guard is live is a data race, same as
/// destroying an object while a raw pointer to it is being dereferenced.
///
/// Protection is also not synchronization. The guard says nothing about data
/// races on the pointee itself; concurrent mutation still needs its own
/// synchronization. It pairs with immutable published snapshots.
///
/// The guard is movable but not copyable: each guard owns exactly one hazard
/// pointer. Moving transfers the protection and leaves the source empty.
template <typename T, template <typename> class Atom>
class hazptr_protected_ptr {
  hazptr_holder<Atom> holder_;
  T* ptr_ = nullptr;

  // Non-owning noexcept reference to a filter functor. std::reference_wrapper
  // cannot be used: its operator() is not noexcept, so it fails protect's
  // nothrow-invocable constraint even when the functor itself is noexcept.
  // Takes and holds by reference; handles invocable objects, function
  // references, and function pointers.
  template <typename Func>
  struct func_ref {
    constexpr T* operator()(T* p) const noexcept {
      return f_(static_cast<T*>(p)); // call with prvalue
    }

    Func& f_;
  };

  // Whether Func is usable as a tagged-pointer filter: nothrow-invocable
  // taking T* and returning T*, and not a member pointer (mirrors
  // hazptr_holder::protect's own constraint).
  template <typename Func>
  static inline constexpr bool nothrow_filter_v =
      std::is_nothrow_invocable_r_v<T*, Func&, T*> &&
      !std::is_member_pointer_v<Func>;

  // Whether Proj is usable as a projection from C* to the viewed T*:
  // nothrow-invocable taking C* and returning T*, not a member pointer, and
  // not the identity case (C == T is the tagged-pointer filter's job, which
  // pins the filtered pointer rather than the container; excluding it here
  // also keeps the two constructors unambiguous).
  template <typename C, typename Proj>
  static inline constexpr bool nothrow_projection_v =
      !std::is_same_v<C, T> && std::is_nothrow_invocable_r_v<T*, Proj&, C*> &&
      !std::is_member_pointer_v<Proj>;

  // Pins the container published by src and returns the projected view, or
  // nullptr when src is empty (proj never sees null).
  template <typename C, typename Proj>
  static T* protect_projected(
      hazptr_holder<Atom>& holder, const Atom<C*>& src, Proj& proj) {
    if (C* p = holder.protect(src)) {
      return proj(static_cast<C*>(p)); // call with prvalue
    }
    return nullptr;
  }

 public:
  /// Default-constructs an empty guard: views nothing, protects nothing.
  hazptr_protected_ptr() noexcept = default;

  /// Protects the object currently published by src. The protection lasts
  /// until this guard is destroyed or moved from.
  explicit hazptr_protected_ptr(const Atom<T*>& src)
      : holder_(make_hazard_pointer<Atom>()), ptr_(holder_.protect(src)) {}

  /// Protects the object currently published by src, mapped through f (for
  /// tagged pointers; f must be nothrow-invocable taking T* and returning
  /// T*, and deterministic: it runs once per protect retry plus once more
  /// to compute the stored pointer). f is taken by value but never copied
  /// or moved again internally.
  template <typename Func>
    requires nothrow_filter_v<Func>
  hazptr_protected_ptr(const Atom<T*>& src, Func f)
      : holder_(make_hazard_pointer<Atom>()),
        ptr_(f(holder_.protect(src, func_ref{f}))) {}

  /// Protects the object currently published by src, holding the hazard
  /// pointer in the given domain. Use this when the object retires into a
  /// non-default domain: protection is honored only by matching in the
  /// domain the holder belongs to.
  hazptr_protected_ptr(hazptr_domain<Atom>& domain, const Atom<T*>& src)
      : holder_(make_hazard_pointer<Atom>(domain)),
        ptr_(holder_.protect(src)) {}

  /// Domain-taking overload of the tagged-pointer constructor above.
  template <typename Func>
    requires nothrow_filter_v<Func>
  hazptr_protected_ptr(hazptr_domain<Atom>& domain, const Atom<T*>& src, Func f)
      : holder_(make_hazard_pointer<Atom>(domain)),
        ptr_(f(holder_.protect(src, func_ref{f}))) {}

  /// Protects the object currently published by src, viewing a projection of
  /// it. The hazard pins the published container p for exactly as long as
  /// the guard lives, while the guard exposes proj(p), which may point
  /// inside p (e.g., a member of a published snapshot). proj runs at most
  /// once, on the pinned value, and never sees null: an empty src yields an
  /// empty guard. proj must be nothrow-invocable taking C* and returning T*.
  /// A generic-lambda proj must declare its return type explicitly: checking
  /// the sibling constructors may instantiate it at the viewed type, where
  /// a deduced return would instantiate the body.
  /// proj is taken by value but never copied or moved again internally.
  template <typename C, typename Proj>
    requires nothrow_projection_v<C, Proj>
  hazptr_protected_ptr(const Atom<C*>& src, Proj proj)
      : holder_(make_hazard_pointer<Atom>()),
        ptr_(protect_projected(holder_, src, proj)) {}

  hazptr_protected_ptr(hazptr_protected_ptr&& other) noexcept
      : holder_(std::move(other.holder_)),
        ptr_(std::exchange(other.ptr_, nullptr)) {}
  hazptr_protected_ptr& operator=(hazptr_protected_ptr&& other) noexcept {
    if (this != &other) {
      holder_ = std::move(other.holder_);
      ptr_ = std::exchange(other.ptr_, nullptr);
    }
    return *this;
  }
  hazptr_protected_ptr(const hazptr_protected_ptr&) = delete;
  hazptr_protected_ptr& operator=(const hazptr_protected_ptr&) = delete;

  ~hazptr_protected_ptr() = default;

  T* get() const noexcept { return ptr_; }
  T* operator->() const noexcept { return ptr_; }
  T& operator*() const noexcept { return *ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

  friend bool operator==(
      const hazptr_protected_ptr& guard, std::nullptr_t) noexcept {
    return guard.get() == nullptr;
  }

  /// Releases the protection early and views nothing. A new guard may be
  /// move-assigned over this one afterwards to protect again.
  void reset() noexcept {
    // Move-assign, not reset_protection: the holder may be empty (default
    // construction, move-from, or an earlier reset), and reset_protection
    // is UB on an empty holder. This also recycles the hazard record.
    *this = hazptr_protected_ptr();
  }
};

} // namespace folly
