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

#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/Utility.h>
#include <folly/coro/safe/AsyncClosure-fwd.h>
#include <folly/lang/SafeAlias-fwd.h>
// `#undef`ed at end-of-file not to leak this macro.
#include <folly/coro/safe/detail/DefineMovableDeepConstLrefCopyable.h>
#include <folly/detail/tuple.h>

///
/// Please read the user- and developer-facing docs in `Capture.md`.
///
/// Callers typically only include `BindCaptures.h`, while callees need to
/// include `Captures.h`.  Both are provided by `AsyncClosure.h`.
///

#if FOLLY_HAS_COROUTINES

namespace folly {
class CancellationToken;
class exception_wrapper;
} // namespace folly

namespace folly::coro {

class AsyncObjectTag;

template <safe_alias, typename>
class safe_task;

namespace detail {

namespace lite_tuple {
using namespace ::folly::detail::lite_tuple;
}

template <typename>
struct AsyncObjectRefForSlot;

template <typename T>
concept has_async_closure_co_cleanup_error_oblivious =
    requires(T t, async_closure_private_t p) { std::move(t).co_cleanup(p); };
template <typename T>
concept has_async_closure_co_cleanup_with_error = requires(
    T t, async_closure_private_t p, const exception_wrapper* e) {
  std::move(t).co_cleanup(p, e);
};
template <typename T> // DO NOT USE: for AsyncObject only
concept has_async_object_private_hack_co_cleanup = requires(
    T t, async_closure_private_t p, const exception_wrapper* e) {
  t.privateHack_co_cleanup(std::move(t), p, e);
};
template <typename T>
concept has_async_closure_co_cleanup =
    has_async_closure_co_cleanup_error_oblivious<T> ||
    has_async_closure_co_cleanup_with_error<T> ||
    has_async_object_private_hack_co_cleanup<T>;
// `T` must be immovable to go in `co_cleanup_capture<T>` and similar places.
// The aim here is to prevent bugs. A safe "move-like" operation for `T` must:
//   - Ensure that the destination of the move is "managed", i.e. is another
//     `co_cleanup_capture<>` or a similar object from `folly/coro/safe`
//     privileged to access `capture_private_t`. Otherwise, cleanup is no longer
//     guaranteed.
//   - Leave the moved-out object in a state where its `co_cleanup()`, which
//     will still be awaited, is a safe no-op.
//
// A regular move ctor cannot adequately vet the destination. That is because
// per `CoCleanupAsyncRAII.md`, a just-constructed object must NEVER require
// cleanup (required since closure setup is fallible, e.g. due to `bad_alloc`).
//
// So, where necessary (I don't have such a use-case yet) -- types should
// provide a specialized move operation instead.
template <typename T>
concept immovable_async_closure_co_cleanup =
    has_async_closure_co_cleanup<T> && !std::is_copy_constructible_v<T> &&
    !std::is_copy_assignable_v<T> && !std::is_move_constructible_v<T> &&
    !std::is_move_assignable_v<T> && !std::swappable<T>;

template <typename, template <typename> class, typename>
class capture_crtp_base;

} // namespace detail

template <typename T>
  requires(!detail::has_async_closure_co_cleanup<T>)
class capture;
template <typename T>
  requires(!detail::has_async_closure_co_cleanup<T>)
class after_cleanup_capture;
template <typename T>
class capture_indirect;
template <typename T>
class after_cleanup_capture_indirect;

// Given a cvref-qualified `capture` type, what `capture` reference type is it
// convertible to?  The input value category affects the output reference type
// exactly as you'd expect for types NOT wrapped by `capture`.  But,
// additionally, this knows to pick the correct wrapper:
//   - `co_cleanup_capture` inputs become `co_cleanup_capture<SomeRef>`
//   - `after_cleanup_ref_*` inputs become `after_cleanup_capture<SomeRef>`
//   - everything else becomes just `capture<SomeRef>`
template <typename Captures>
using capture_ref_conversion_t =
    std::remove_cvref_t<Captures>::template ref_like_t<Captures>;

// This namespace has tools for library authors who're building new
// `co_cleanup` types. See the guide in `Captures.md`.
namespace ext {

// Used with `capture_proxy(capture_proxy_tag<KIND>, ...)`.  We don't
// need to track `const` state here, since prvalue semantics do apply any
// `const` qualifier on the return type of `capture_proxy()`.
enum class capture_proxy_kind {
  lval_ref,
  lval_ptr,
  rval_ref,
  rval_ptr,
};

// Passkey used with `capture_proxy` methods.
template <capture_proxy_kind Kind>
class capture_proxy_tag {
 private:
  template <typename, template <typename> class, typename>
  friend class ::folly::coro::detail::capture_crtp_base;
  explicit capture_proxy_tag() = default;
};

// When implementing the `capture_proxy()` ADL customization point, it is
// important for the second argument to match both `T&` and `const T&`:
//   template <capture_proxy_kind Kind, const_or_not<YourType> Me>
//   friend auto capture_proxy(capture_proxy_tag<Kind>, Me&);
template <typename T, typename U>
concept const_or_not = (std::same_as<T, U> || std::same_as<T, const U>);

} // namespace ext

namespace detail {

// DANGER: Using this passkey makes it easy to break the lifetime-safety
// guarantees of `folly/coro/safe`, so before adding a new callsite, get
// familiar with the lifetime-safety docs, and get a careful review.  This used
// to have a private-with-friends constructor, but the friend list grew
// unmanageably large as the number of lifetime-safe utilities increased.
struct capture_private_t {
  explicit capture_private_t() = default;
};

struct capture_restricted_tag {}; // detail of `restricted_co_cleanup_capture`

template <typename T>
struct bind_wrapper_t {
  T t_;
  constexpr decltype(auto) what_to_bind() && { return static_cast<T&&>(t_); }
};

// Makes a `bind_wrapper_t` with a forwarding ref of the argument.
constexpr auto forward_bind_wrapper(auto&& v [[clang::lifetimebound]]) {
  static_assert(std::is_reference_v<decltype(v)>);
  return bind_wrapper_t<decltype(v)>{static_cast<decltype(v)>(v)};
}

constexpr auto unsafe_tuple_to_bind_wrapper(auto tup) {
  static_assert(1 == std::tuple_size_v<decltype(tup)>);
  return bind_wrapper_t<std::tuple_element_t<0, decltype(tup)>>{
      .t_ = lite_tuple::get<0>(std::move(tup))};
}

template <typename Derived, template <typename> class RefArgT, typename T>
class capture_crtp_base {
 private:
  static constexpr decltype(auto) assert_result_is_non_copyable_non_movable(
      auto&& fn) {
    using U = decltype(fn());
    // Tests `U&` instead of `is_copy_*` to catch non-regular classes that
    // declare a U(U&) ctor.  This implementation is for class types only.
    static_assert(
        // E.g. `AsyncObjectPtr::capture_proxy()` just returns a reference
        // or pointer, in effect emulating `capture_indirect`.
        std::is_reference_v<U> || std::is_pointer_v<U> ||
            !(std::is_constructible_v<U, U&> ||
              std::is_constructible_v<U, U&&> || std::is_assignable_v<U&, U&> ||
              std::is_assignable_v<U&, U&&>),
        "When a class provides custom dereferencing via `capture_proxy`, "
        "it must be `NonCopyableNonMovable` to ensure that it can only passed "
        "via `capture<Ref>`, not via your temporary proxy object. The goals "
        "are (1) ensure correct `safe_alias_of` markings, (2) keep the "
        "forwarding object as a hidden implementation detail.");
    return fn();
  }

  // Object intended for use with `capture`  (like `safe_async_scope`) may
  // provide overloads of the helper function `capture_proxy` to provide
  // proxy types for `capture` operators `*` and `->`.
  //
  // IMPORTANT: Be sure to cover the options in `capture_proxy_kind`.  Also,
  // if you provide a `const`-qualified `capture_proxy` it should model
  // `const` access.
  //
  // The reason for this indirection is as follows:
  //   - "Restricted" references to scopes must enforce stricter
  //     `safe_alias_of` constraints on their awaitables.
  //     `restricted_co_cleanup_capture` explains the usage.
  //   - A `restricted_co_cleanup_capture<Ref>` may be obtained from an
  //     `co_cleanup_capture<...AsyncScope...>` that was originally NOT
  //     restricted -- so, "restricted" is a property of the reference, not
  //     of the underlying scope object.
  //   - Therefore, the public API of `safe_async_scope` must sit in a
  //     "reference" object that knows if it's restricted, not in the storage
  //     object (which does not).
  //   - It would break encapsulation to put `AsyncScope`-specific logic like
  //     `add` / `schedule` / `schedule*Closure` into `Captures.h`.
  //
  // A type will not be accessible via `restricted_co_cleanup_capture`
  // unless it provides overloads for `capture_restricted_proxy`.  There's
  // no default behavior for restricted refs, because the underlying class
  // needs to implement strong enough safety constraints that the ref can be
  // `after_cleanup_ref`.
  template <ext::capture_proxy_kind Kind>
  static constexpr decltype(auto) get_proxy(
      ext::capture_proxy_tag<Kind> proxy_tag, auto& self) {
    auto& lref = self.get_lref();
    if constexpr (std::is_base_of_v<capture_restricted_tag, Derived>) {
      return assert_result_is_non_copyable_non_movable([&]() -> decltype(auto) {
        return capture_restricted_proxy(proxy_tag, lref);
      });
    } else if constexpr ( // Custom dereference
        requires { capture_proxy(proxy_tag, lref); }) {
      return assert_result_is_non_copyable_non_movable([&]() -> decltype(auto) {
        return capture_proxy(proxy_tag, lref);
      });
    } else if constexpr (Kind == ext::capture_proxy_kind::lval_ref) {
      return lref; // Unproxied l-value reference
    } else if constexpr (Kind == ext::capture_proxy_kind::rval_ref) {
      // Implement regular forwarding-ref semantics:
      //   (V&)&& -> V&, (V)&& -> V&&, (V&&)&& -> V&&
      if constexpr (std::is_lvalue_reference_v<T>) {
        return lref;
      } else {
        return std::move(lref);
      }
    } else if constexpr (
        Kind == ext::capture_proxy_kind::lval_ptr ||
        Kind == ext::capture_proxy_kind::rval_ptr) {
      return &lref; // Unproxied pointer
    } else {
      static_assert(false, "Unhandled capture_proxy_kind");
    }
  }

  // Invokes a callable, ensuring its return value is of type `Expected`,
  // while retaining prvalue semantics.
  template <typename Expected>
  static constexpr auto assert_return_type(auto fn) {
    static_assert(std::is_same_v<decltype(fn()), Expected>);
    return fn();
  }

 public:
  using capture_type = T;

  // Implement operators `*` and `->` for lvalue `capture` types.
  //
  // This rvalue specialization has an intentional & important deviation in
  // semantics:
  //   - All the getters require a `&&`-qualified object, i.e.  their intended
  //     use is destructive -- you can `*std::move(arg_ref)` once.  Thereafter,
  //     use-after-move linters will complain if you reuse the `capture<V&&>`.
  //   - Correspondingly, `operator*` returns `V&&` instead of `V&`.
  [[nodiscard]] constexpr decltype(auto) operator*() & noexcept {
    static_assert(
        !std::is_rvalue_reference_v<T>,
        "With `capture<T&&> a`, use `*std::move(a)`");
    return get_proxy(
        ext::capture_proxy_tag<ext::capture_proxy_kind::lval_ref>{},
        *static_cast<Derived*>(this));
  }
  [[nodiscard]] constexpr decltype(auto) operator*() && noexcept {
    return get_proxy(
        ext::capture_proxy_tag<ext::capture_proxy_kind::rval_ref>{},
        *static_cast<Derived*>(this));
  }
  [[nodiscard]] constexpr decltype(auto) operator->() & noexcept {
    static_assert(
        !std::is_rvalue_reference_v<T>,
        "With `capture<T&&> a`, use `std::move(a)->`");
    return get_proxy(
        ext::capture_proxy_tag<ext::capture_proxy_kind::lval_ptr>{},
        *static_cast<Derived*>(this));
  }
  [[nodiscard]] constexpr decltype(auto) operator->() && noexcept {
    return get_proxy(
        ext::capture_proxy_tag<ext::capture_proxy_kind::rval_ptr>{},
        *static_cast<Derived*>(this));
  }
  [[nodiscard]] constexpr decltype(auto) operator*() const& noexcept {
    static_assert(
        !std::is_rvalue_reference_v<T>,
        "With `capture<T&&> a`, use `*std::move(a)`");
    return get_proxy(
        ext::capture_proxy_tag<ext::capture_proxy_kind::lval_ref>{},
        *static_cast<const Derived*>(this));
  }
  [[nodiscard]] constexpr decltype(auto) operator*() const&& noexcept {
    return get_proxy(
        ext::capture_proxy_tag<ext::capture_proxy_kind::rval_ref>{},
        *static_cast<const Derived*>(this));
  }
  [[nodiscard]] constexpr decltype(auto) operator->() const& noexcept {
    static_assert(
        !std::is_rvalue_reference_v<T>,
        "With `capture<T&&> a`, use `std::move(a)->`");
    return get_proxy(
        ext::capture_proxy_tag<ext::capture_proxy_kind::lval_ptr>{},
        *static_cast<const Derived*>(this));
  }
  [[nodiscard]] constexpr decltype(auto) operator->() const&& noexcept {
    return get_proxy(
        ext::capture_proxy_tag<ext::capture_proxy_kind::rval_ptr>{},
        *static_cast<const Derived*>(this));
  }

  // Private implementation detail -- public users should instead use the below
  // conversions.  This is how `async_closure` (and similar) create a matching
  // `capture<Ref>` from a `Derived` instance.  The resulting type is
  // `RefArgT`, except for the narrow case when a non-`shared_cleanup` closure
  // is converting a `after_cleanup_ref_` input.
  //   - `Derived::capture_type` may be a value or a reference
  //   - `T` may be a value or reference
  // The main reason `to_capture_ref` is locked down is that when
  // `SharedCleanupClosure == false`, we upgrade `after_cleanup_ref_` refs.
  // This is unsafe to do unless we know that the ref is going into
  // an independent, nested async scope.
  template <bool SharedCleanupClosure>
  auto to_capture_ref(capture_private_t) & {
    return to_capture_ref_impl<SharedCleanupClosure>(
        static_cast<Derived&>(*this).get_lref());
  }
  template <bool SharedCleanupClosure>
  auto to_capture_ref(capture_private_t) const& {
    return to_capture_ref_impl<SharedCleanupClosure>(
        static_cast<const Derived&>(*this).get_lref());
  }
  template <bool SharedCleanupClosure>
  auto to_capture_ref(capture_private_t) && {
    return to_capture_ref_impl<SharedCleanupClosure>(
        std::move(static_cast<Derived&>(*this).get_lref()));
  }
  template <bool SharedCleanupClosure>
  auto to_capture_ref(capture_private_t) const&& {
    return to_capture_ref_impl<SharedCleanupClosure>(
        std::move(static_cast<const Derived&>(*this).get_lref()));
  }

  // Prefer `capture_ref_conversion_t`, which is easier to use.  Given an
  // instance of this `capture` of cvref category `LikeMe`, which
  // `capture<Ref>` can it be converted to?
  template <typename LikeMe>
  using ref_like_t = RefArgT<like_t<LikeMe&&, T>>;

  // Convert a capture instance to a capture reference of a matching cvref
  // category.
  //
  // Two implicit conversions are provided because we want capture-wrapped
  // types to act much like the underlying unwrapped types.  You can think of
  // this conversion as allowing cvref qualifiers on the wrapper to be moved
  // **inside** the wrapper.  The test shows full coverage, but in essence,
  // the outer reference category replaces the inner one, any `const` moves
  // inside the wrapper, and we never remove a `const` qualifier already
  // present in the wrapper.  Examples:
  //   capture<int>& -> capture<int&>
  //   const capture<int>& -> capture<const int&>
  //
  // The rvalue qualified analog is explicit, to avoid some bad side effects:
  //   capture<const int&>&& -> capture<const int&&> (explicit!)
  //
  // For those 3 conversions, find the destination `capture` type via the
  // function `capture_ref_conversion_t`.
  //
  // We also support an explicit rref to lref conversion:
  //   capture<int&&>&& -> capture<int&>
  // The idea here is that you're passing `capture<V&&>` down into a child
  // of your closure.  That deliberately has stricter single-use semantics
  // than `V&&` in vanilla C++ -- for example, without single-use, an rref
  // could be used to move out a value that is still referenced in
  // safe_async_scope task.  Having the explicit && -> & conversion permits
  // the child change its mind about moving out the value.
  //
  // Future ideas & implementation notes:
  //   - We may want to support implicitly adding `const`. Today's solution
  //     is to take `const capture`, which should be fine for most usage?
  //   - This (and `to_capture_ref` should technically have a `const&&`
  //     overload, but that's "impact for another day", whenever someone
  //     actually needs it.
  //   - All 3 of these conversions can be `operator auto`, but I suspect
  //     this would hurt compile-time.  Benchmark before changing.
  /*implicit*/ operator ref_like_t<int&>() & {
    return assert_return_type<ref_like_t<int&>>([&] {
      return static_cast<Derived&>(*this)
          .template to_capture_ref</*SharedCleanup*/ true>(capture_private_t{});
    });
  }
  /*implicit*/ operator ref_like_t<const int&>() const& {
    return assert_return_type<ref_like_t<const int&>>([&] {
      return static_cast<const Derived&>(*this)
          .template to_capture_ref</*SharedCleanup*/ true>(capture_private_t{});
    });
  }
  // This is explicit, because if it were implicit, then prvalues of type
  // `capture<int&>` would bind to arguments of type `capture<int&&>` which is
  // an unexpected / unsafe behavior.
  explicit operator auto() && { // Actually, `operator ref_like_t<int&&>`
    // This has to be `operator auto`, with a "stub" branch for cleanup
    // args, because an `co_cleanup_capture` constraint bans r-value
    // references, preventing us from unconditionally instantiating
    // `ref_like_t<int&&>` for all `capture` types.  It would be possible to
    // delay the "no rvalue reference" test by making it a `static_assert`
    // in a constructor (or another guaranteed-to-be-instantiated) function,
    // but this wouldn't be shorter, and it would be more fragile.
    if constexpr (has_async_closure_co_cleanup<std::remove_cvref_t<T>>) {
      return;
    } else {
      return assert_return_type<ref_like_t<int&&>>([&] {
        return static_cast<Derived&&>(*this)
            .template to_capture_ref</*SharedCleanup*/ true>(
                capture_private_t{});
      });
    }
  }
  // Allow explicitly moving `capture<V&&>` into `capture<V&>`. Example:
  //   auto lcap = capture<int&>{std::move(rcap)};
  explicit operator ref_like_t<int&>() &&
    requires(std::is_rvalue_reference_v<T>)
  {
    return assert_return_type<ref_like_t<int&>>([&] {
      return to_capture_ref_impl</*SharedCleanup*/ true>(
          static_cast<Derived&&>(*this).get_lref());
    });
  }

 private:
  template <bool SharedCleanupClosure, typename V>
  static auto to_capture_ref_impl(V&& v) {
    // If the receiving closure takes no `shared_cleanup` args, then it
    // cannot* pass any of its `capture` refs to an external, longer-lived
    // cleanup callback.  That implies we can safely upgrade any incoming
    // `after_cleanup_ref_` refs to regular post-cleanup `capture` refs --
    // anything received from the parent is `co_cleanup_safe_ref` from the point
    // of view of **this** closure's cleanup args, and it cannot access others.
    //
    // * As always, subject to the `SafeAlias.h` caveats.
    if constexpr (has_async_closure_co_cleanup<V>) {
      // Identical to the default `else` branch.  Required, since we cannot
      // instantiate `after_cleanup_capture<V>` when `V` has `co_cleanup`.
      return RefArgT<V&&>{
          capture_private_t{}, forward_bind_wrapper(static_cast<V&&>(v))};
    } else if constexpr (
        !SharedCleanupClosure &&
        std::is_same_v<RefArgT<V>, after_cleanup_capture<V>>) {
      return capture<V&&>{
          capture_private_t{}, forward_bind_wrapper(static_cast<V&&>(v))};
    } else if constexpr (
        !SharedCleanupClosure &&
        std::is_same_v<RefArgT<V>, after_cleanup_capture_indirect<V>>) {
      return capture_indirect<V&&>{
          capture_private_t{}, forward_bind_wrapper(static_cast<V&&>(v))};
    } else {
      return RefArgT<V&&>{
          capture_private_t{}, forward_bind_wrapper(static_cast<V&&>(v))};
    }
  }
};

// The primary template is for values, with a specialization for references.
// Value and lval refs should quack the same, exposing a pointer-like API,
// which (unlike regular pointers or ref wrappers) is deep-const.
//
// The rvalue reference specialization has a nonstandard semantics.  For
// `capture`s, rvalue refs are **single-use**.  Users should only create
// `capture<V&&>` if they intend to move the value, or perform another
// destructive operation.
//
// Why specialize for references instead of storing `T t_;` in a single
// class, and dispatch via SFINAE?  The main reason is that `T t_` wouldn't
// support assignment, since `T = V&` or `T = V&&` could not be rebound.
template <typename Derived, template <typename> class RefArgT, typename V>
class capture_storage : public capture_crtp_base<Derived, RefArgT, V> {
  static_assert(!std::is_reference_v<V>); // Specialized for refs below
 public:
  constexpr capture_storage(capture_private_t, auto bind_wrapper)
      : v_(std::move(bind_wrapper).what_to_bind()) {}

 protected:
  template <typename, template <typename> class, typename>
  friend class capture_crtp_base;
  friend void async_closure_set_cancel_token(
      async_closure_private_t, auto&&, const CancellationToken&);
  friend auto async_closure_make_cleanup_tuple(
      async_closure_private_t, auto&&, const exception_wrapper*);
  template <typename> // For the `capture` specializations only!
  friend struct AsyncObjectRefForSlot;
  template <typename ArgMap, size_t ArgI, typename Arg>
  friend decltype(auto) async_closure_resolve_backref(
      capture_private_t, auto&, Arg&);

  constexpr auto& get_lref() noexcept { return v_; }
  constexpr const auto& get_lref() const noexcept { return v_; }

  V v_;
};
// Future: When `R` is an rvalue reference, it might be good to support a
// runtime check against reuse, in the style of `RValueReferenceWrapper`.
// Unlike that class, I would make it DFATAL to avoid opt-build cost.
template <typename Derived, template <typename> class RefArgT, typename R>
  requires std::is_reference_v<R>
class capture_storage<Derived, RefArgT, R>
    : public capture_crtp_base<Derived, RefArgT, R> {
 public:
  // This double-cast is an ugly workaround to go from `V&&` to `V*`.  We
  // need the outer `const_cast` because C++ doesn’t allow address-of-rvalue
  // refs, and only allows them to be cast to `const` lvalue refs.  It is
  // safe, since the final destination type has the same const-qualification
  // as the original `what_to_bind()` result.
  constexpr capture_storage(capture_private_t, auto bind_wrapper)
      : p_(&const_cast<std::remove_reference_t<R>&>(
            static_cast<std::add_const_t<std::remove_reference_t<R>>&>(
                std::move(bind_wrapper).what_to_bind()))) {}

 protected:
  template <typename, template <typename> class, typename>
  friend class capture_crtp_base;
  constexpr auto& get_lref() noexcept { return *p_; }
  constexpr const auto& get_lref() const noexcept { return *p_; }

  std::remove_reference_t<R>* p_;
};

// There are no "heap reference" variants since a reference doesn't need to
// know how it's stored, and "heap" vs "plain" is meant to be a low-visibility
// implementation detail.
template <typename Derived, template <typename> class RefArgT, typename T>
  requires(!std::is_reference_v<T> && !has_async_closure_co_cleanup<T>)
// Since `capture_heap` is owned directly by the inner task, it has to be
// movable to be passed to the coroutine.  But, to stay API-compatible per
// above, it'd be preferable if users did NOT move it.  To help prevent such
// moves, a linter is proposed in `FutureLinters.md`.
//
// We deliberately do NOT support moving out the underlying `unique_ptr`
// because heap storage is meant to be an implementation detail, and is not
// intended to be nullable.  A user needing nullability should pass a
// `unique_ptr` either as `capture_indirect` (1 dereference) or `capture` (2).
class capture_heap_storage : public capture_crtp_base<Derived, RefArgT, T> {
 public:
  capture_heap_storage(capture_private_t, auto bind_wrapper)
      : p_(std::make_unique<T>(std::move(bind_wrapper).what_to_bind())) {}

 protected:
  template <typename, template <typename> class, typename>
  friend class capture_crtp_base;
  constexpr auto& get_lref() noexcept { return *p_; }
  constexpr const auto& get_lref() const noexcept { return *p_; }

  std::unique_ptr<T> p_;
};

// This is a direct counterpart to `capture_storage` that collapses two
// dereference operations into one for better UX.  There is no need for a
// `capture_heap_indirect_storage`, since this "indirect" syntax sugar only
// applies to pointer types, which are always cheaply movable, and thus
// don't benefit from `bind::in_place`.
//
// Similarly, no support for `co_cleanup()` captures since those generally
// aren't pointer-like, and won't suffer from double-dereferences.
template <typename Derived, template <typename> class RefArgT, typename T>
  requires(!has_async_closure_co_cleanup<T>)
class capture_indirect_storage : public capture_storage<Derived, RefArgT, T> {
 public:
  using capture_storage<Derived, RefArgT, T>::capture_storage;

  // These are all intended to be equivalent to dereferencing the
  // corresponding `capture<T>` twice.
  [[nodiscard]] constexpr decltype(auto) operator*() & noexcept {
    return *(capture_storage<Derived, RefArgT, T>::operator*());
  }
  [[nodiscard]] constexpr decltype(auto) operator*() const& noexcept {
    return *(capture_storage<Derived, RefArgT, T>::operator*());
  }
  [[nodiscard]] constexpr decltype(auto) operator*() && noexcept {
    return *(
        std::move(*this).capture_storage<Derived, RefArgT, T>::operator*());
  }
  [[nodiscard]] constexpr decltype(auto) operator*() const&& noexcept {
    return *(
        std::move(*this).capture_storage<Derived, RefArgT, T>::operator*());
  }
  [[nodiscard]] constexpr decltype(auto) operator->() & noexcept {
    return (capture_storage<Derived, RefArgT, T>::operator->()) -> operator->();
  }
  [[nodiscard]] constexpr decltype(auto) operator->() const& noexcept {
    return (capture_storage<Derived, RefArgT, T>::operator->()) -> operator->();
  }
  [[nodiscard]] constexpr decltype(auto) operator->() && noexcept {
    return (std::move(*this).capture_storage<Derived, RefArgT, T>::operator->())
        -> operator->();
  }
  [[nodiscard]] constexpr decltype(auto) operator->() const&& noexcept {
    return (std::move(*this).capture_storage<Derived, RefArgT, T>::operator->())
        -> operator->();
  }

  // Unlike other captures, `capture_indirect` is nullable since the
  // underlying pointer type is, too.
  explicit constexpr operator bool() const
      noexcept(noexcept(this->get_lref().operator bool())) {
    return this->get_lref().operator bool();
  }

  // Use these to access the underlying `T`, instead of dereferencing twice.
  //
  // RISKS: Clearing or reallocating a pointer (e.g. `reset()`) in async
  // code can cause faults for other code that holds a `capture` reference.
  // Ideally, you should only use this if you can prove that there are no
  // other outstanding references, or that they all expect the change.
  decltype(auto) get_underlying_unsafe() & {
    return capture_storage<Derived, RefArgT, T>::operator*();
  }
  decltype(auto) get_underlying_unsafe() const& {
    return capture_storage<Derived, RefArgT, T>::operator*();
  }
  decltype(auto) get_underlying_unsafe() && {
    return std::move(capture_storage<Derived, RefArgT, T>::operator*());
  }
  decltype(auto) get_underlying_unsafe() const&& {
    return std::move(capture_storage<Derived, RefArgT, T>::operator*());
  }
};

// `capture_safety_impl_v` is separate for `AsyncObject.h` to specialize
template <typename T, safe_alias Default>
inline constexpr auto capture_safety_impl_v = safe_alias_of<T, Default>::value;

// ALL "capture" types must have `folly_private_safe_alias_t` markings.
//
// `capture` refs are only valid as long as their on-closure storage.  They can
// be copied/moved, so their `safe_alias` marking is the only thing preventing
// the use of invalid references.  The docs in `enum class safe_alias` discuss
// how safety levels are assigned for closure `capture`s.  `async_closure`
// invokes `to_capture_ref()` to emit refs with the appropriate safety.
//
// If the underlying type is `<= shared_cleanup`, that leaks through to
// all `capture`s containing it.  See e.g. `AsyncObjectPtr`.
//   * Note: A `shared_cleanup` type `T` gives a closure a way of passing refs
//     onto parent `safe_async_scope`s (generically: cleanup phases), so
//     `capture<T>` must never be safer than `T` (unless we're dealing with a
//     restricted capture ref),
//
// Otherwise, the safety measurement of `T` is "outer" to the current
// closure, and is one of `after_cleanup_ref`, `co_cleanup_safe_ref`, or
// `maybe_value`.  Those should all behave the same inside the closure,
// so `MaxRefSafety` is all that matters.
//   * Note: `capture<V>` is convertible to `capture<V&>` etc, so the ref
//     version should never be safer.
template <typename T, safe_alias MaxRefSafety, safe_alias Default>
struct capture_safety
    : safe_alias_constant<
          (capture_safety_impl_v<std::remove_reference_t<T>, Default> <=
           safe_alias::shared_cleanup)
              ? std::min(
                    MaxRefSafety,
                    capture_safety_impl_v<std::remove_reference_t<T>, Default>)
              : MaxRefSafety> {};

} // namespace detail

// Please read the file docblock.
//
// Rationale for the move/copy policy of `A = capture<T>`:
//   - When `T` is a ref, `A` must be passed-by-value into coroutines, and
//     so must be at least movable.
//   - Ideally, for value `T`, the args would be permanently attached to the
//     originating closure, but we have to let them be movable so that
//     `async_closure`s without the outer task can own them.  To help
//     prevent this, a linter is proposed in `FutureLinters.md`.
//   - Forbid copying for rvalue ref `T` to make use-after-move linters useful.
//     We don't follow `folly::rvalue_reference_wrapper` in adding a runtime
//     `nullptr` check for moved-out refs, but this could be done later.
//   - Allowing copies of lvalue refs is optional, but helpful.  For example,
//     it lets users naturally pass arg refs into bare sub-tasks.  This seems
//     like a reasonable & low-risk thing to do -- our operators already expose
//     refs to the underlying data, so we can't prevent the user from passing
//     `T&` to non-`safe_alias` callables, anyhow.
template <typename T> // may be a value or reference
  requires(!detail::has_async_closure_co_cleanup<T>)
class capture : public detail::capture_storage<capture<T>, capture, T> {
 public:
  FOLLY_MOVABLE_AND_DEEP_CONST_LREF_COPYABLE(capture, T);
  using detail::capture_storage<capture<T>, capture, T>::capture_storage;
  template <safe_alias Default>
  using folly_private_safe_alias_t =
      detail::capture_safety<T, safe_alias::co_cleanup_safe_ref, Default>;
};
template <typename T> // may be a value or reference
  requires(!detail::has_async_closure_co_cleanup<T>)
class after_cleanup_capture
    : public detail::
          capture_storage<after_cleanup_capture<T>, after_cleanup_capture, T> {
 public:
  FOLLY_MOVABLE_AND_DEEP_CONST_LREF_COPYABLE(after_cleanup_capture, T);
  using detail::capture_storage<
      after_cleanup_capture<T>,
      after_cleanup_capture,
      T>::capture_storage;
  template <safe_alias Default>
  using folly_private_safe_alias_t =
      detail::capture_safety<T, safe_alias::after_cleanup_ref, Default>;
};

// The use-case for `capture_heap` is to allow a closure without cleanup
// args to avoid an inner/outer task split, while still taking
// `bind::in_place` arguments.  This is meant to be an implementation detail
// that's almost fully API-compatible with `capture`.  At a future
// point we *could* remove this:
//  - Then, any use of `bind::in_place` would auto-create an outer task.
//  - Any user code that explicitly specifies `capture_heap` in signatures
//    would need to be updated to `capture`.
//  - Any places that rely on moving `capture_heap<V>` would need to migrate
//    to `capture_indirect<std::unique_ptr<V>>{}` (which, in contrast, is
//    nullable).  This should be rare, since we mark all value `capture`s as
//    `unsafe` to encourage leaving the value `capture` wrappers in-closure.
template <typename T>
class capture_heap
    : public detail::capture_heap_storage<capture_heap<T>, capture, T> {
 public:
  using detail::capture_heap_storage<capture_heap<T>, capture, T>::
      capture_heap_storage;
  template <safe_alias Default>
  using folly_private_safe_alias_t =
      detail::capture_safety<T, safe_alias::co_cleanup_safe_ref, Default>;
};
template <typename T>
class after_cleanup_capture_heap
    : public detail::capture_heap_storage<
          after_cleanup_capture_heap<T>,
          after_cleanup_capture,
          T> {
 public:
  using detail::capture_heap_storage<
      after_cleanup_capture_heap<T>,
      after_cleanup_capture,
      T>::capture_heap_storage;
  template <safe_alias Default>
  using folly_private_safe_alias_t =
      detail::capture_safety<T, safe_alias::after_cleanup_ref, Default>;
};

// `capture_indirect<SomePtr<T>>` is like `capture<SomePtr<T>>` with syntax
// sugar to avoid dereferencing twice.  Use `get_underlying_unsafe()` instead
// of `*` / `->` to access the pointer object itself (see its doc for RISKS).
template <typename T>
class capture_indirect
    : public detail::
          capture_indirect_storage<capture_indirect<T>, capture_indirect, T> {
 public:
  using detail::capture_indirect_storage<
      capture_indirect<T>,
      capture_indirect,
      T>::capture_indirect_storage;
  template <safe_alias Default>
  using folly_private_safe_alias_t =
      detail::capture_safety<T, safe_alias::co_cleanup_safe_ref, Default>;
};
template <typename T>
class after_cleanup_capture_indirect
    : public detail::capture_indirect_storage<
          after_cleanup_capture_indirect<T>,
          after_cleanup_capture_indirect,
          T> {
 public:
  using detail::capture_indirect_storage<
      after_cleanup_capture_indirect<T>,
      after_cleanup_capture_indirect,
      T>::capture_indirect_storage;
  template <safe_alias Default>
  using folly_private_safe_alias_t =
      detail::capture_safety<T, safe_alias::after_cleanup_ref, Default>;
};

// A closure that takes a cleanup arg is required to mark its directly-owned
// `capture`s with the `after_cleanup_` prefix, to prevent refs to these
// short-lived args from being passed into longer-lived callbacks. Similarly,
// it may not upgrade incoming `after_cleanup_capture`s to just `capture`s.
//
// Don't allow r-value refs to cleanup args, since moving those out of the
// owning closure is unexpected, and probably wrong.
template <typename T> // may be a value or lvalue reference
  requires(!std::is_rvalue_reference_v<T> &&
           detail::immovable_async_closure_co_cleanup<std::remove_cvref_t<T>>)
class co_cleanup_capture
    : public detail::
          capture_storage<co_cleanup_capture<T>, co_cleanup_capture, T>,
      std::conditional_t<
          !std::is_reference_v<T>,
          folly::NonCopyableNonMovable,
          tag_t<>> {
 public:
  FOLLY_MOVABLE_AND_DEEP_CONST_LREF_COPYABLE(co_cleanup_capture, T);
  using detail::capture_storage<co_cleanup_capture<T>, co_cleanup_capture, T>::
      capture_storage;
  template <safe_alias Default>
  using folly_private_safe_alias_t =
      detail::capture_safety<T, safe_alias::shared_cleanup, Default>;
};

// What this accomplishes, in brief -- details in `Captures.md`:
//   - A closure that takes a `co_cleanup_capture<X&> x` from a parent will
//     see some of its arguments downgraded to `after_cleanup_capture`.
//   - To avoid the safety downgrade, the closure can instead take the ref
//     as `restricted_co_cleanup_capture<X&> xr`, whose APIs will mirror
//     those of `x`, but will be restricted to ONLY accept args with
//     `maybe_value` safety.
//
// This only takes `T = V&`, because "restricted" is always a view on
// an underlying `co_cleanup_capture`.
//
// `V` needs to ADL-customize `capture_restricted_proxy()`.
template <typename T>
  requires(std::is_lvalue_reference_v<T> &&
           detail::immovable_async_closure_co_cleanup<std::remove_cvref_t<T>>)
class restricted_co_cleanup_capture
    : public detail::capture_storage<
          restricted_co_cleanup_capture<T>,
          restricted_co_cleanup_capture,
          T>,
      private detail::capture_restricted_tag {
 public:
  FOLLY_MOVABLE_AND_DEEP_CONST_LREF_COPYABLE(restricted_co_cleanup_capture, T);
  using detail::capture_storage<
      restricted_co_cleanup_capture<T>,
      restricted_co_cleanup_capture,
      T>::capture_storage;
  // FIXME: `capture_safety` will still measure this as `shared_cleanup` due
  // to `T` being that safety.  So, when implementing restricted refs, we'll
  // have to add a new case to `capture_safety` to handle this.
  template <safe_alias Default>
  using folly_private_safe_alias_t =
      detail::capture_safety<T, safe_alias::after_cleanup_ref, Default>;
};

namespace detail {
template <typename T>
concept is_any_co_cleanup_capture =
    (is_instantiation_of_v<co_cleanup_capture, T> ||
     is_instantiation_of_v<restricted_co_cleanup_capture, T>);
template <typename T>
concept is_any_capture =
    (is_instantiation_of_v<capture, T> ||
     is_instantiation_of_v<capture_heap, T> ||
     is_instantiation_of_v<capture_indirect, T> ||
     is_instantiation_of_v<after_cleanup_capture, T> ||
     is_instantiation_of_v<after_cleanup_capture_heap, T> ||
     is_instantiation_of_v<after_cleanup_capture_indirect, T> ||
     is_instantiation_of_v<co_cleanup_capture, T> ||
     is_instantiation_of_v<restricted_co_cleanup_capture, T>);
template <typename T>
concept is_any_capture_ref =
    is_any_capture<T> && std::is_reference_v<typename T::capture_type>;
template <typename T>
concept is_any_capture_val =
    is_any_capture<T> && !std::is_reference_v<typename T::capture_type>;

} // namespace detail

} // namespace folly::coro

// Customize `safe_closure` to store `capture<V>` as `capture<V&>`.
namespace folly::detail {
template <typename ST>
struct safe_closure_arg_storage_helper;
template <coro::detail::is_any_capture_val ST>
struct safe_closure_arg_storage_helper<ST> {
  // We should never move `capture<Val>`s, so `safe_closure` will fail to
  // implicitly convert from `capture<Val>&&` to `capture<V&>` since the
  // `&&`-qualified conversions are `explicit` above.
  using type = coro::capture_ref_conversion_t<ST&>;
};
} // namespace folly::detail

#endif

#undef FOLLY_MOVABLE_AND_DEEP_CONST_LREF_COPYABLE
