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

#include <compare>

#include <folly/coro/safe/BindCaptures.h>
#include <folly/coro/safe/Captures.h>
#include <folly/lang/SafeAlias.h>
#include <folly/lang/bind/Named.h> // See test `AsyncClosure.captureBackref`
#include <folly/lang/bind/NamedToStorage.h>

/// This header's `async_closure_safeties_and_bindings` implements the
/// argument-binding logic for `async_closure`.
///
/// Before reading further, make sure to get familiar with:
///   - `folly/lang/Bindings.md` for `bind::args` & friends.
///   - `docs/Captures.md` to understand the `capture` type wrappers, and the
///     safety upgrade/downgrade rules for passing them into closures.
/// In particular, know this distinction:
///   * "owned captures" look like `capture<V>`.  These are wrappers tied
///     to the closure whose `bind::capture()` created it. Note that a closure
///     with an outer coro will pass these as `capture<V&>` to the inner task.
///   * "capture references" are `capture<V&>` or `<V&&>, implicitly created
///     for the closure from any caller-provided `capture` (value or ref).
///
/// `async_closure` takes user-specified closure arguments as `bind::args`, an
/// immovable object with 1-expression lifetime.  This header plumbs them
/// through some transformations.  In opt builds, it is intended to be elided
/// by compiler's alias analysis (NB: this needs benchmarks & perhaps tweaks).
///
/// Every variant of `async_closure` invokes `..._safeties_and_bindings()`,
/// which does several jobs.  Here's a summary of the data flow:
///   * Figure out if the closure needs an outer coro, or if it can be elided.
///   * Measure the safety of the closure, as seen by its caller.  This uses
///     the safeties of the arguments **before** any transformations.
///   * Transform the arg tuple:
///       - Convert each entry of the `bind::args` into a `capture` ref (when
///         the caller gave us a `capture`), or one of 4 tag types
///         (`async_closure*_arg` or `async_closure*_self_ref_hack`).  The tag
///         types tells the `async_closure` implementation whether to store the
///         arg, and how to bind it to the inner closure.
///       - Figure out the storage type for each `bind::capture` binding using
///         `bind::bind_to_storage_policy`, to support `bind::in_place*`.
///       - Transform non-owned `capture`s via `to_capture_ref`.  Parents'
///         owned captures are implicitly passed by-ref; `after_cleanup_` refs
///         are "upgraded" if possible.  Docs in `Captures.md`.
///       - Apply special handling to the first argument when the closure runs
///         `FOLLY_INVOKE_MEMBER`.
///       - Other args are perfectly forwarded.
///   * Validate the user inputs and try to issue readable error messages.
///     Also check internal invariants.
///
/// After the validation & transformation, `detail/AsyncClosure.h` is
/// responsible for actually storing the args, and creating the coroutine.
///
/// ## Implementation glossary
///
/// This header classifies the bound arguments into a few categories:
///   * "owned capture" or just "own": Make a new `capture` whose storage (and
///     cleanup) belongs to this closure.
///       - These correspond to `async_closure_{inner,outer}_stored_arg`.
///       - If the closure is shared-cleanup, the safety of the new capture
///         is downgraded to `after_cleanup_ref`.
///       - The inner task sees a `capture` ref for closures with an outer
///         coro, and a `capture` value otherwise.
///   * "pass capture ref" or just "pass": A `capture` from the caller.
///       - These are passed as `capture<Ref>`, even if the input is a value.
///       - The inner coro may see upgraded safety relative to the caller.
///   * "regular arg": The easy / normal case -- simply bind a forwarding
///     reference from the caller to the inner coro.  The reference is dressed
///     in `async_closure_regular_arg`.
///   * "self-reference hack": See `async_closure_scope_self_ref_hack`.

#if FOLLY_HAS_COROUTINES
FOLLY_PUSH_WARNING
FOLLY_DETAIL_LITE_TUPLE_ADJUST_WARNINGS

// Future: If this is used anywhere else, move it to `BindCapturesToStorage.h`.
//
// We extended `bind::ext::bind_info_t` with `capture_kind`, so we must
// explicitly specialize `bind_to_storage_policy`.  We reuse the standard
// rules.  Custom `capture` binding logic lives in `BindAsyncClosure.h`.
namespace folly::bind::ext {
template <auto BI, typename BindingType>
  requires std::same_as< // Written as a constraint to prevent object slicing
      decltype(BI),
      ::folly::bind::detail::capture_bind_info_t>
class bind_to_storage_policy<binding_t<BI, BindingType>> {
 private:
  using standard =
      bind_to_storage_policy<binding_t<bind_info_t{BI}, BindingType>>;

 public:
  using storage_type = typename standard::storage_type;
  using signature_type = typename standard::signature_type;
};
} // namespace folly::bind::ext

namespace folly::coro {
class AsyncObject;
class AsyncScopeSlotObject;
template <typename, size_t>
class safe_async_scope_context_proxy;
template <typename>
class AsyncObjectNonSlotPtr;
} // namespace folly::coro

namespace folly::coro::detail {

template <safe_alias... Vs>
constexpr safe_alias vtag_least_safe_alias(vtag_t<Vs...>) {
  return std::min({safe_alias::maybe_value, Vs...});
}

//
// There are 4 tag types here, which all quack the same interface:
//   * `storage_type`: For storing "owned captures", but also for measuring
//     "caller's point-of-view" safety of regular args.
//   * `bindWrapper_`: A `bind_wrapper_t<T>`, which is literally just `T` but
//     preserving the value category even for references.  It has either the
//     forwarding reference or the value being bound.  Note that `T` is not
//     necessarily related to `storage_type` -- for in-place construction, it
//     is a "maker", which is implicitly-convertible to the `storage_type`.
//
// There's also 5th case without a tag type -- when passing capture refs,
// `async_closure_safeties_and_bindings()` simply emits an unwrapped `capture`,
// and `AsyncClosure.h` detects it via `is_any_capture<Bs>`.
//
// The goal is that none of the 5 cases instantiate any storage, or call
// copy/move constructors until the final moment, when we either:
//   - pass the arg to the inner coro, or
//   - store it in the outer coro's `unique_ptr<tuple<>>` of owned captures.
//

// This is just a fancy forwarding reference, never a value.
template <typename Storage, typename BindWrapper>
struct async_closure_regular_arg {
  using storage_type = Storage;
  BindWrapper bindWrapper_;
};

// Use `is_base_of` since `instantiation_of` cannot handle no-type templates
class async_closure_outer_stored_arg_base {};
template <typename T>
concept is_async_closure_outer_stored_arg =
    std::is_base_of_v<async_closure_outer_stored_arg_base, T>;

// For a given closure, all `_stored_arg` tags are going to be of one flavor.
// With an outer coro, we capture info required to resolve backrefs.  Also, the
// "has outer coro" decision isn't exported in any way besides the "inner" vs
// "outer" stored arg type.  If useful, we could easily refactor this to be one
// `_owned_capture` tag type, and branch on `has_outer_coro` in
// `AsyncClosure.h`.  Reworking things this way would make it possible to have
// an outer coro without storage, which might be needed if someone has a
// legitimate use-case for captures that define `setParentCancelToken()` or
// `setParentExecutor()` without defining `co_cleanup()` (dubious!).
template <is_any_capture Storage, typename BindWrapper, size_t ArgI, auto Tag>
struct async_closure_outer_stored_arg : async_closure_outer_stored_arg_base {
  using storage_type = Storage;
  constexpr static inline size_t arg_idx = ArgI;
  constexpr static inline auto tag = Tag;
  BindWrapper bindWrapper_;
};
template <is_any_capture Storage, typename BindWrapper>
struct async_closure_inner_stored_arg {
  using storage_type = Storage;
  BindWrapper bindWrapper_;
};

// ## Why does this `self_ref_hack` type even exist?
//
// To avoid synchronization costs, `folly::coro` async scopes disallow `add()`
// after `joinAsync()` has completed (violating this is UB).  However, it is
// always safe to call `add()` on an async scope from a task running on that
// **same** scope, even if `joinAsync()` has already started. This is because
// any active scope awaitable prevents its `joinAsync()` from completing.
//
// Unfortunately, though it would be safe, you cannot directly pass an
// `async_arg_cleanup<Scope&>` ref (`shared_cleanup` safety) into a closure
// being scheduled on the SAME async scope (needs `>= co_cleanup_safe_ref`).
// That's because at compile-time, we cannot tell if the "scope being scheduled
// on" is the same as "the scope being referenced".
//
// Moreover, "pass a ref to the current scope" is the ONLY case that is always
// safe [1].  Otherwise, the closure with the scope ref could run `add()` when
// the (other) scope that it references had already been cleaned up.
//
// [1] Future: Thanks to `co_cleanup` ordering, it may later be feasible to
// allow scopes that are passed later (cleaned up earlier) to reference those
// that are passed earlier (cleaned up later).
//
// ## How does `self_ref_hack` help allow safe self-references for scopes?
//
// The solution is to introduce `scheduleScopeClosure()`, which acts just like
// `schedule(async_closure())`, but prepends an "implicit scope parameter" to
// the user-supplied `bind::args{}`.
//
// This implicit param is `self_ref_hack`, which is handled specially inside
// `detail/AsyncClosure*`.  As a result, the user's inner task gets a
// `co_cleanup_capture<Scope&>` as its first argument.
//
// Analogously, `AsyncObject::scheduleSelfClosure()` uses this mechanism to
// allow sub-closures to safely reference the `Slot` of the object, which
// contains the "current" async scope.
//
// ## Why are all aspects of `self_ref_hack` protected?
//
// This is NOT safe to use in other settings, because:
// (1) We instantiates a new `capture` ref from a bare reference, bypassing the
//     usual lifetime safety checks.
// (2) This `ref_hack` object is deliberately excluded from the final
//     `async_closure`'s safety accounting -- it only affects the
//     `shard_cleanup` downgrade.
// Both of these are ONLY okay because the sub-closure's own scope outlives it.
template <typename Storage, typename BindWrapper>
struct async_closure_scope_self_ref_hack {
  using storage_type = Storage;

 protected:
  template <size_t, typename Bs, bool>
  friend decltype(auto) async_closure_bind_inner_coro_arg(
      capture_private_t, Bs&, auto&);
  template <typename, size_t>
  friend class folly::coro::safe_async_scope_context_proxy;
  friend class folly::coro::AsyncScopeSlotObject;
  // The innards are `protected`, since `transform_binding` lets this be
  // supplied from outside the closure machinery.  Any new client being
  // added here must think THOROUGHLY about the risks in the class docblock!
  explicit async_closure_scope_self_ref_hack(BindWrapper b)
      : bindWrapper_(std::move(b)) {}
  BindWrapper bindWrapper_;
};

struct binding_helper_cfg {
  bool is_shared_cleanup_closure;
  bool has_outer_coro;
  bool in_safety_measurement_pass;
  constexpr auto operator<=>(const binding_helper_cfg&) const = default;
};

template <typename Binding, auto Cfg, size_t ArgI>
class capture_binding_helper;

struct capture_ref_measurement_stub {};

// This helper class only has static members.  It exists only so that the
// various functions can share some type aliases.
template <
    std::derived_from<bind::ext::bind_info_t> auto BI,
    typename BindingType,
    auto Cfg,
    size_t ArgI>
class capture_binding_helper<bind::ext::binding_t<BI, BindingType>, Cfg, ArgI> {
 private:
  // A constraint on the template would make forward-declarations messy.
  static_assert(std::is_same_v<decltype(Cfg), binding_helper_cfg>);

  using category_t = bind::ext::category_t;
  using ST = typename bind::ext::bind_to_storage_policy<
      bind::ext::binding_t<BI, BindingType>>::storage_type;
  using UncvrefST = std::remove_cvref_t<ST>;

  // "Pass capture ref" validation.  Here, `ST` is either a value or a
  // reference `capture`.  `RetST` is the `capture<Ref>` for the inner task.
  //
  // There is no "business logic" here.  This only documents the possible data
  // flows, `static_assert`s a few common user errors for better compiler
  // messages, and refers to the relevant unit tests.
  template <typename RetST>
  static constexpr void static_assert_passing_capture() {
    using ArgT = typename UncvrefST::capture_type;
    static_assert(std::is_reference_v<ST> == (BI.category == category_t::ref));
    if constexpr (std::is_reference_v<ArgT>) { // Is `capture<Ref>`?
      // Design note: Why do we automatically pass all `capture`s by-reference?
      //
      // As an alternative, recall we have `bind::const_ref` / `bind::mut_ref`.
      // These are now used for `bind::capture_const_ref` and
      // `bind::capture_mut_ref` with different semantics (below).  Why didn't
      // we instead use these as mandatory markings for `captures` that get
      // passed by-reference.  That might seem more explicit, but also more
      // confusing and harder to use:
      //   - `const_ref` / `mut_ref CANNOT be used for "regular" args -- they're
      //     by-reference iff the caller writes `T&` in the signature.
      //   - `capture`s are intended to belong to the parent closure, it rarely
      //     makes sense to copy or move them.
      //   - Syntactically, `capture`s behave like pointers.
      //   - If we needed an explicit `const_ref` / `mut_ref` only to pass
      //     `capture<Val>` as a `capture<Ref>`, then migrating a closure from
      //     "has outer coro" to "lacks outer coro" would require adding such a
      //     modifier at every callsite.
      //   - You can still move out the contents of a capture into a child by
      //     passing an rvalue ref as `std::move(cap)` to the child, or by
      //     passing the actual value via `*std::move(cap)`.  Similarly, `*cap`
      //     would copy the value.
      //
      // N.B.  We DO use `bind::capture{const_ref{}}` etc in order to convert
      // plain references from a parent coro into `capture` refs in a child
      // closure, see "capture-by-reference" in `Captures.md`.
      static_assert(
          !std::is_reference_v<ST>,
          "Pass `capture<Ref>` by value, do not use `const_ref` / `mut_ref`");
      // Passing the caller's `capture<Ref>` makes a new `capture` ref object.
      if constexpr (std::is_lvalue_reference_v<BindingType>) {
        // Improve errors over just "deleted copy ctor".
        static_assert(
            !std::is_rvalue_reference_v<ArgT>,
            "capture<V&&> is move-only. Try std::move(yourRef).");

        // `check_capture_lref_to_lref` tests this branch -- passing a
        // `capture<Ref>` that the caller bound as an lvalue.  We'll copy it,
        // potentially upgrading `after_cleanup_capture` -> `capture.
      } else {
        // Cleanup args don't support rval refs.  No user-facing message, since
        // `co_cleanup_capture` would first have a constraint failure.
        static_assert(!is_any_co_cleanup_capture<UncvrefST>);

        // Tests: `check_capture_lref_to_rref` & `check_capture_rref_to_rref`.
        // An input `capture<Ref>` bound as an rvalue gives the child a
        // `capture<Val&&>`.
        //
        // It is in some sense optional to support this, since users can
        // pass around lval refs and `std::move(*argRef)` at the last
        // minute.  However, I wanted to encourage the best practice of
        // `std::move(argRef)` at the outermost callsite that knows about
        // the move.  Reasons:
        //   - The initial `std::move(arg)` enables use-after-move linting
        //     in the outermost scope.
        //   - `capture<T&&>` is move-only, meaning subsequent scopes also
        //     get use-after-move linting.
        //   - Future: we could build a debug-only use-after-move runtime
        //     checker by adding some state on `capture`s.
      }
    } else { // Is `capture<Val>`?
      // Cleanup args require an outer coro, so it should never be the case
      // that `*co_cleanup_capture<Val>` is being passed to a child..
      static_assert(!is_any_co_cleanup_capture<UncvrefST>);

      // Tested in `check_capture_val_to_ref`: `capture<Val>` is implicitly
      // passed as `capture<Ref>`.
    }
  }

  template <typename T>
  static constexpr auto store_as(auto bind_wrapper) {
    if constexpr (Cfg.has_outer_coro) {
      return async_closure_outer_stored_arg<
          T,
          decltype(bind_wrapper),
          ArgI,
          bind::ext::named_bind_info_tag_v<decltype(BI)>>{
          .bindWrapper_ = std::move(bind_wrapper)};
    } else {
      return async_closure_inner_stored_arg<T, decltype(bind_wrapper)>{
          .bindWrapper_ = std::move(bind_wrapper)};
    }
  }

  // "owned capture": The closure creates storage for `bind::capture()` bindings
  static constexpr auto store_capture_binding(auto bind_wrapper) {
    static_assert(
        !is_any_capture<ST>,
        "Given a capture `c`, do not write `bind::capture(c)` to pass it to a "
        "closure. Just write `c` as the argument, and it'll automatically "
        "be passed as a capture reference.");
    if constexpr (has_async_closure_co_cleanup<ST>) {
      static_assert(Cfg.has_outer_coro);
      // Future: Add a toggle to emit `restricted_co_cleanup_capture`
      return store_as<co_cleanup_capture<ST>>(std::move(bind_wrapper));
    } else if constexpr (
        BI.captureKind_ == bind::detail::capture_kind::indirect) {
      if constexpr (Cfg.is_shared_cleanup_closure) {
        return store_as<after_cleanup_capture_indirect<ST>>(
            std::move(bind_wrapper));
      } else {
        return store_as<capture_indirect<ST>>(std::move(bind_wrapper));
      }
    } else if constexpr (
        !Cfg.has_outer_coro &&
        // `bind::in_place*` is often used for immovable types, so without an
        // outer coro, they must be on-heap to pass ownership to the inner coro.
        bind::ext::is_binding_t_type_in_place<BindingType> &&
        // Heuristic: Moving a type is usually cheaper than putting it on
        // the heap.  If not, people can always use `capture_indirect` with
        // `unique_ptr`...  Or, we could later add new capture kinds, like
        // `plain_auto_storage = 0`, `plain_heap`, and `plain_non_heap`.
        !std::is_move_constructible_v<BindingType>) {
      if constexpr (Cfg.is_shared_cleanup_closure) {
        return store_as<after_cleanup_capture_heap<ST>>(
            std::move(bind_wrapper));
      } else {
        return store_as<capture_heap<ST>>(std::move(bind_wrapper));
      }
    } else {
      if constexpr (Cfg.is_shared_cleanup_closure) {
        return store_as<after_cleanup_capture<ST>>(std::move(bind_wrapper));
      } else {
        return store_as<capture<ST>>(std::move(bind_wrapper));
      }
    }
  }

  template <typename>
  static inline constexpr bool is_supported_capture_bind_info_v = false;

  template <>
  inline constexpr bool
      is_supported_capture_bind_info_v<bind::detail::capture_bind_info_t> =
          true;

  // Future: Right now, we only check that `"x"_id = ` tags are unique at time
  // of use, and this only applies for stored captures.  But, from a pure "code
  // quality" point of view, it would be reasonable to demand that all tags are
  // unique, and that they are all used.  This could be done either as a linter
  // or in this file, at some compile-time cost.
  template <auto Tag>
  static inline constexpr bool is_supported_capture_bind_info_v<
      bind::ext::named_bind_info_t<Tag, bind::detail::capture_bind_info_t>> =
      true;

 public:
  // Transforms the binding as per the file docblock, returns a new binding.
  // (either one of the 4 tag types above, or `capture<Ref>`)
  static constexpr auto transform_binding(auto bind_wrapper) {
    if constexpr (is_supported_capture_bind_info_v<decltype(BI)>) {
      // Implement "capture-by-reference", docs in `Captures.md`
      if constexpr (BI.category == category_t::ref) {
        // Test in `check_parent_capture_ref`
        static_assert(std::is_reference_v<ST>);
        static_assert(
            !is_any_capture<UncvrefST>,
            "Do not use `const_ref` / `mut_ref` verbs to pass a `capture` to "
            "a child closure -- just pass it directly.");
        // It should be hard to get a ref to a co_cleanup type
        static_assert(!has_async_closure_co_cleanup<UncvrefST>);
        if constexpr (Cfg.in_safety_measurement_pass) {
          return capture_ref_measurement_stub{};
        } else if constexpr (Cfg.is_shared_cleanup_closure) {
          return after_cleanup_capture<ST>{
              capture_private_t{}, std::move(bind_wrapper)};
        } else {
          return capture<ST>{capture_private_t{}, std::move(bind_wrapper)};
        }
      } else { // Tests in `check_stored_*`.
        static_assert(!std::is_reference_v<ST>);
        return store_capture_binding(std::move(bind_wrapper));
      }
    } else { // Bindings for arguments the closure does NOT store.
      static_assert(
          std::is_same_v<vtag_t<BI>, vtag_t<bind::ext::bind_info_t{}>>,
          "`folly::bind::` modifiers like `constant` (or `\"x\"_id = `) "
          "only make sense with `bind::capture()` bindings -- for example, to "
          "move a mutable value into `const` capture storage. For regular "
          "args, use `const` in the signature of your inner coro, and/or "
          "`std::as_const` when passing the arg.");
      // If we allowed `bind::in_place` without `bind::capture`, the argument
      // would require a copy or a move to be passed to the inner task (which
      // the type may not support).  If `bind::capture` isn't appropriate, the
      // user can also work around that via `std::make_unique<TheirType>` and/or
      // `bind::capture_indirect`.
      static_assert(
          !bind::ext::is_binding_t_type_in_place<BindingType>,
          "Did you mean `bind::capture_in_place<T>(...)`?");
      if constexpr (is_any_capture<UncvrefST>) { // Tests in `check_capture_*`
        // Pass preexisting `capture`s (NOT owned by this closure).
        // Future: Add a toggle to make `restricted_co_cleanup_capture` refs.
        auto arg_ref =
            std::move(bind_wrapper)
                .what_to_bind()
                .template to_capture_ref<Cfg.is_shared_cleanup_closure>(
                    capture_private_t{});
        static_assert_passing_capture<decltype(arg_ref)>();
        return std::move(arg_ref);
      } else if constexpr (
          is_instantiation_of_v<async_closure_scope_self_ref_hack, UncvrefST>) {
        // This `ref_hack` type quacks like the `stored_arg` types, but we need
        // to unwrap it for it to be handled correctly downstream.
        return std::move(bind_wrapper).what_to_bind();
      } else { // Test in `check_regular_args`
        // "regular" args -- neither an owned capture (`bind::capture()` et al),
        // nor a parent's `capture`. Passed via forwarding reference.

        // This may be redundant, since `co_cleanup_capture` enforces that
        // cleanup types are immovable.  If we did allow passing bare
        // `co_cleanup` types, it could violate memory safety protections for
        // `async_closure`s.  For `async_now_closure`, there is also no obvious
        // use-case for passing `*captureVar` by-reference into the child.
        static_assert(
            !has_async_closure_co_cleanup<UncvrefST>,
            "This argument implements `async_closure` cleanup, so you should "
            "almost certainly pass it `bind::capture()` -- or, if you already "
            "have as a reference `capture`, by-value.");

        return async_closure_regular_arg<ST, decltype(bind_wrapper)>{
            .bindWrapper_ = std::move(bind_wrapper)};
      }
    }
  }
};

// See `vtag_safety_of_async_closure_args` for the docs.
// NB: As a nested lambda, this breaks on clang-17 due to compiler bugs.
template <bool ParentViewOfSafety, typename T>
auto vtag_safety_of_async_closure_arg() {
  // "owned capture": `store_as` outputs `async_closure_*_stored_arg`.
  if constexpr (
      is_async_closure_outer_stored_arg<T> ||
      is_instantiation_of_v<async_closure_inner_stored_arg, T>) {
    using CT = typename T::storage_type::capture_type;
    static_assert(!std::is_reference_v<CT>);
    // Stored captures are as safe as the type being stored.  For example, when
    // a closure stores a `BackgroundTask<Safety, T>`, it cannot be safer than
    // `Safety`.
    //
    // While this replicates `lenient_safe_alias_of_v` logic, we don't directly
    // use it here, since `AsyncObject.h` specializes `capture_safety_impl_v`.
    return vtag<capture_safety_impl_v<CT, safe_alias::maybe_value>>;
  } else if constexpr ( //
      is_instantiation_of_v<async_closure_scope_self_ref_hack, T>) {
    // This is a closure made by `spawn_self_closure()` et al. It must:
    //  - Avoid marking the closure's outer task `shared_cleanup`, so it can
    //    still be added to the scope that made it (`if` branch).
    //  - Downgrade [*] the safety of its own captures (`else` branch).
    //
    // [*] It would be memory-unsafe to reference such captures from
    // recursively scheduled closures on the same scope!
    if constexpr (ParentViewOfSafety) {
      return vtag<>;
    } else {
      constexpr auto storage_safety =
          lenient_safe_alias_of_v<typename T::storage_type>;
      // In current usage, ref_hack can only contain `co_cleanup_capture<V&>`.
      static_assert(storage_safety == safe_alias::shared_cleanup);
      return vtag<storage_safety>;
    }
  } else if constexpr (is_any_capture<T>) {
    // "pass capture ref": Output of the `to_capture_ref` branch.
    static_assert(std::is_reference_v<typename T::capture_type>);
    return vtag<lenient_safe_alias_of_v<T>>;
  } else if constexpr (std::is_same_v<capture_ref_measurement_stub, T>) {
    if constexpr (ParentViewOfSafety) {
      // Only allow capture-by-reference in `async_now_closure`s
      return vtag<safe_alias::unsafe>;
    } else {
      // But, don't do closure-internal downgrades, since `transform_bindings`
      // tries to prevent it from taking in refs to co_cleanup types this way.
      return vtag<>;
    }
  } else {
    // "regular arg": A non-`capture` passed via forwarding reference.
    static_assert(is_instantiation_of_v<async_closure_regular_arg, T>);
    return vtag<lenient_safe_alias_of_v<typename T::storage_type>>;
  }
}

// Returns a vtag of `safe_alias_v` for the storage type of the args that
// did not come from `store_capture_binding`.
//
// We have to special-case the stored ones because `Captures.h` marks the
// `capture` wrappers for on-closure stored values `unsafe` to discourage users
// from moving them from the original closure.  And, the wrappers themselves
// check the safety of the underlying type (via `capture_safety`).
//
// The doc in `scheduleScopeClosure()` justifies why our first call to this
// function includes `ref_hack` args in the measurement (we want the
// closure's own args downgraded to `after_cleanup_ref` safety), but not in
// the second (we don't want the emitted `safe_task` to be knocked down to
// `shared_cleanup` safety, since that would make it unschedulable).
template <bool ParentViewOfSafety, typename TransformedBindingList>
constexpr auto vtag_safety_of_async_closure_args() {
  return []<typename... T>(tag_t<T...>) {
    return value_list_concat_t<
        vtag_t,
        decltype(vtag_safety_of_async_closure_arg<
                 ParentViewOfSafety,
                 T>())...>{};
  }(TransformedBindingList{});
}

template <typename BindingT>
constexpr bool capture_needs_outer_coro() {
  using BP = bind::ext::bind_to_storage_policy<BindingT>;
  using ST = typename BP::storage_type;
  return has_async_closure_co_cleanup<ST>;
}

struct async_closure_bindings_cfg {
  bool force_outer_coro;
  bool emit_now_task;
  bool is_invoke_member;
};

// For `is_invoke_member` closures, we must run an additional lifetime-safety
// check.  For convenience, we also implicitly wrap the first argument with
// `bind::capture` when that's the obviously right choice.
template <async_closure_bindings_cfg Cfg>
struct async_closure_invoke_member_bindings {
  constexpr auto operator()(tag_t<>) { return tag<>; }
  template <auto BI0, typename BT0, auto... BI, typename... BT>
  constexpr auto operator()(
      tag_t<bind::ext::binding_t<BI0, BT0>, bind::ext::binding_t<BI, BT>...>) {
    using T = std::remove_cvref_t<BT0>;
    constexpr bool arg0_is_non_owning_ptr =
        // `transform_binding()` passes captures as non-owning refs
        is_any_capture<T> ||
        // Raw pointers are allowed in `async_now_closure()`
        std::is_pointer_v<T> ||
        is_instantiation_of_v<folly::coro::AsyncObjectNonSlotPtr, T> ||
        // `scheduleScopeClosure` & `scheduleSelfClosure` give non-owning
        // pointers.  NB: This covers `SlotLimitedObjectPtr`.
        is_instantiation_of_v<async_closure_scope_self_ref_hack, T>;
    // Invoking a `member_task` requires `force_outer_coro` iff the first arg
    // is an owning capture.
    //
    // NB: Both implicit & explicit `bind::capture()`s are assumed to be owning,
    // and thus also `force_outer_coro`.
    //
    // The reason that `force_outer_coro` is NOT done automatically is that
    // it adds perf overhead, which would be easily avoided if the user made
    // their member function `static` instead.
    static_assert(
        Cfg.force_outer_coro || !Cfg.is_invoke_member || arg0_is_non_owning_ptr,
        "It looks like you want the `member_task` closure to own the object "
        "instance. Use `async_now_closure(bind::args{&obj}, fn)` if that "
        "applies. The next best approach is to make your task `static`, "
        "with its first arg `auto self`. If that's not viable, then use "
        "`async_closure_config{.force_outer_coro = true}` to allocate a "
        "coro frame to own your object.");
    // Syntax sugar: `bind::capture()` may be left as implicit for the arg0
    // "object parameter" of `FOLLY_INVOKE_MEMBER`.
    if constexpr (
        Cfg.is_invoke_member &&
        // If arg0 is `bind::capture()` or similar, don't double-wrap it.
        !std::derived_from<decltype(BI0), bind::detail::capture_bind_info_t> &&
        // Non-owning pointer-like things don't need to be captured.
        !arg0_is_non_owning_ptr) {
      static_assert(
          // BT0 is a value for `bind::in_place`, rval ref otherwise.
          !std::is_lvalue_reference_v<BT0>,
          "If you call `async_closure` with `FOLLY_INVOKE_MEMBER` and "
          "a non-`capture` argument, then it has to be an r-value, so "
          "that the closure can take ownership of the object instance. "
          "Consider `folly::copy()` or `std::move()`.");
      return tag<
          bind::ext::binding_t<
              bind::detail::capture_bind_info<
                  bind::detail::capture_kind::plain>{}(BI0),
              BT0>,
          bind::ext::binding_t<BI, BT>...>;
    } else {
      return tag<
          bind::ext::binding_t<BI0, BT0>,
          bind::ext::binding_t<BI, BT>...>;
    }
  }
};

template <safe_alias ParentView, bool ArgsForceSharedCleanup>
struct async_closure_arg_safety {
  static inline constexpr auto parent_view = ParentView;
  static inline constexpr auto args_force_shared_cleanup =
      ArgsForceSharedCleanup;
};

// Converts forwarded arguments to bindings, figures out the storage policy
// (outer coro?, shared cleanup?), and applies `transform_bindings` to compute
// the final storage & binding outcome for each argument.  The caller should
// create an outer coro iff the resulting `tuple` contains at least one
// `async_closure_outer_stored_arg`.
//
// Returns a 3-tuple:
// [0] `async_closure_arg_safety`, see `vtag_safety_of_async_closure_arg()`
// [1] A tuple `transform_bindings()` outputs, containing:
//       binding OR async_closure_{inner,outer}_stored_arg, ...
//     This tuple is always used by `bind_captures_to_closure`, at least for
//     type computations, if not for creating the actual inner coro.  It is
//     computed as if the closure's shared-cleanup behavior were fully
//     determined by the bound args we see here.
// [2] Either `nullptr`, OR a tuple of `transform_bindings()` outputs identical
//     to [1], except that shared-cleanup is force-enabled.  This tuple is only
//     computed for the cases when `bind_captures_to_closure` would use it.
//
// NB: It's fine for this implementation detail to take `BoundArgs` by-ref
// because `async_closure` & friends took them by value.
template <async_closure_bindings_cfg Cfg, typename BoundArgs>
constexpr auto async_closure_safeties_and_bindings(BoundArgs&& bargs) {
  using Bindings = decltype(async_closure_invoke_member_bindings<Cfg>{}(
      typename BoundArgs::binding_list_t{}));

  auto tup = static_cast<BoundArgs&&>(bargs).unsafe_tuple_to_bind();
  auto make_bindings = [&]<binding_helper_cfg HelperCfg>(vtag_t<HelperCfg>) {
    return [&]<size_t... Is>(std::index_sequence<Is...>) {
      return lite_tuple::tuple{[&]() {
        using Binding = type_list_element_t<Is, Bindings>;
        using T = std::tuple_element_t<Is, decltype(tup)>;
        return capture_binding_helper<Binding, HelperCfg, Is>::
            transform_binding(
                bind_wrapper_t<T>{
                    .t_ = static_cast<T&&>(lite_tuple::get<Is>(tup))});
      }()...};
    }(std::make_index_sequence<type_list_size_v<Bindings>>{});
  };

  // Future: If there are many `bind::in_place` arguments (which require
  // `capture_heap`), it may be more efficient to auto-select an outer coro,
  // for just 2 heap allocations.  Beware: this changes user-facing types
  // (`capture_heap` to `capture`), but most users shouldn't depend on that.
  constexpr bool has_outer_coro =
      Cfg.force_outer_coro || []<typename... Bs>(tag_t<Bs...>) {
        return (capture_needs_outer_coro<Bs>() || ...);
      }(Bindings{});
  // Figure out `IsSharedCleanupClosure` for `binding_cfg` for the real
  // `transform_binding` call.
  //
  // Our choice of `is_shared_cleanup_closure = true` is important since we
  // reuse this type list for the returned
  // `vtag_safety_of_async_closure_args`.  That vtag is used by
  // `async_closure` to compute the safety level for the resulting
  // `safe_task`.  This safety must NOT be increased by reference upgrades --
  // a reference's safety is only upgraded inside the child closure, but the
  // original safety applies in the parent closure, which is where the
  // returned `vtag` is consumed.
  //
  // Choosing `true` here does not affect the `is_shared_cleanup` choice below
  // It merely toggles between `after_cleanup_ref_capture` and `capture`, with
  // either `after_cleanup_ref` or `co_cleanup_safe_ref` safety.
  using shared_cleanup_transformed_binding_types = type_list_concat_t<
      tag_t,
      decltype(make_bindings(
          vtag<binding_helper_cfg{
              .is_shared_cleanup_closure = true,
              .has_outer_coro = has_outer_coro,
              .in_safety_measurement_pass = true}>))>;
  // The template args store two views of the safety of the closure's args:
  //   - As seen by the parent, where `ParentViewOfSafety == true`,
  //   - As used for internal safety computations.
  //
  // This toggle supports two usage scenarios:
  //
  // (1) Capture-by-reference behaviors, like `bind::capture_const_ref()` /
  // `bind::capture(const_ref())` et al.
  //    - `unsafe` for parent --  Since these are raw references from the
  //      parent's scope, ensure they're only allowed in `async_now_closure`s.
  //    - Ignored by child -- Simultaneously, we don't want the internal coro
  //      to be subject to shared-cleanup downgrades.  Doing that would,
  //      e.g., break the useful pattern of an on-closure scope collecting
  //      results on a parent collector passed via capture-by-reference.
  //
  // (2) Closures created by `spawn_self_closure()` et al.  Also see
  // `async_closure_scope_self_ref_hack`.
  //
  //   - In the returned `vtag` that measures the parent's view of the safety
  //     of the closure, `ParentViewOfSafety == true` will exclude the
  //     closure's first arg (the scope or object ref) from the vtag -- it
  //     would otherwise be `shared_cleanup`.  That is, of course, the entire
  //     point of `spawn_self_closure()` -- we happen to know that the scope
  //     ref is safe because of the circumstances of the closure's creation.
  //
  //   - Using `ParentViewOfSafety = false` here makes `spawn_self_closure`s
  //     **internally** consider themselves to be `shared_cleanup` closures.
  //     I.e. `after_cleanup_` inputs are not upgraded, and owned captures are
  //     downgraded to `after_cleanup_`.
  //
  //     To see that these downgrades are the correct behavior, imagine a chain
  //     of closures, each calling `spawn_self_closure()` to make the next.
  //     `safe_async_scope` awaits these concurrently, so they must not take
  //     dependencies on each other's owned captures.
  async_closure_arg_safety<
      // parent_view=
      vtag_least_safe_alias(
          vtag_safety_of_async_closure_args<
              /*ParentViewOfSafety*/ true,
              shared_cleanup_transformed_binding_types>()),
      // args_force_shared_cleanup=
      (safe_alias::shared_cleanup >=
       vtag_least_safe_alias(
           vtag_safety_of_async_closure_args<
               /*ParentViewOfSafety*/ false,
               shared_cleanup_transformed_binding_types>()))>
      arg_min_safety;

  // How this async-closure will store and/or bind its arguments depends on
  // whether we have to force shared-cleanup downgrades for its args.
  //
  // This can happen in two ways:
  //
  // (1) `async_closure` takes a `safe_task` and emits a `safe_task`.  Just
  //     above, we computed` `args_force_shared_cleanup`, which says that the
  //     closure needs shared-cleanup iff a parent passes a
  //     `co_cleanup_capture` ref.
  static_assert(
      safe_alias::closure_min_arg_safety == safe_alias::shared_cleanup);

  // (2) `async_now_closure` always emits a `now_task`, which is meant
  //     to be usable even with unsafe coros & arguments.
  //
  //     However, we only need shared-cleanup if the inner-coro COULD access a
  //     ref of safety <= `shared_cleanup`.  Above, we tested the explicit
  //     inputs via `args_force_shared_cleanup`, but that misses the "implicit
  //     object parameter" (aka `this`) of the coro callable.
  //
  //     For example, `async_now_closure(..., [&]() -> now_task<> { ...  })`
  //     compiles, and can access potentially-unsafe refs via the lambda's
  //     `this` pointer, which gets access to raw refs into the outer scope.
  //
  //     Since `make_inner_coro` is evaluated synchronously, it is allowed to
  //     be a "coro wrapper", i.e. a plain lambda with unsafe captures that
  //     calls an actual coroutine function.  We only care about the safety of
  //     the resulting coro.  Luckily, `is_safe_task_valid` from `SafeTask.h`
  //     constrains the safety of the implicit object parameter.  Specifically,
  //     we know that a `safe_task` with `ArgSafety >= unsafe_internal_closure`
  //     requires[*] the implicit object parameter to have safety `>
  //     shared_cleanup` (yes, greater than!), which is exactly our goal here.
  //
  //     To sum up: Above, we saw that if `async_now_closure`'s inner coro has
  //     safety `>= unsafe_closure_internal`, then it should not have access to
  //     any refs of safety `<= shared_cleanup`.
  //
  //     HOWEVER, here, in `async_closure_safeties_and_bindings`, we do not yet
  //     know the inner coro result type, and cannot test its safety.  In fact,
  //     we need to synthesize the argument types for `make_inner_coro` just to
  //     resolve the right overload.  As a workaround, the returned 3-tuple
  //     provides bindings for BOTH options -- by tuple index:
  //
  //     [1] `task_forces_shared_cleanup == false`.  Use these arg bindings,
  //         made with `.is_shared_cleanup == args_force_shared_cleanup`, to
  //         compute the inner coro type.  Use them to actually make the inner
  //         coro ONLY IF it has safety `>= unsafe_closure_internal`.
  //
  //     [2] `task_forces_shared_cleanup == true`.  These arg bindings are
  //         computed optionally (condition below), by setting
  //         `.is_shared_cleanup == true`.  They will be used to make a
  //         fallback inner coro if the plan from [1] didn't work.  We force
  //         shared cleanup because, per above, the unsafe inner coro may have
  //         access to unsafe refs.
  auto maybe_make_bindings_for =
      [&]<bool task_forces_shared_cleanup>(vtag_t<task_forces_shared_cleanup>) {
        if constexpr (
            // Always run `make_bindings` for index 1 of the returned `tuple`.
            !task_forces_shared_cleanup ||
            // Only run `make_bindings` for index 2 IF it may get used.
            (Cfg.emit_now_task && !arg_min_safety.args_force_shared_cleanup)) {
          return make_bindings(
              vtag<binding_helper_cfg{
                  .is_shared_cleanup_closure = task_forces_shared_cleanup ||
                      arg_min_safety.args_force_shared_cleanup,
                  .has_outer_coro = has_outer_coro,
                  .in_safety_measurement_pass = false}>);
        } else {
          return nullptr; // result[2] will not be used
        }
      };

  return lite_tuple::tuple{
      arg_min_safety,
      // The doc above `maybe_make_bindings_for` explains how these are used.
      maybe_make_bindings_for(vtag</*task_forces_shared_cleanup*/ false>),
      maybe_make_bindings_for(vtag</*task_forces_shared_cleanup*/ true>)};
}

} // namespace folly::coro::detail

FOLLY_POP_WARNING
#endif
