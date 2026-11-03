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

#include <type_traits>

#include <folly/Utility.h>
#include <folly/lang/SafeAlias-fwd.h>

#include <folly/detail/tuple.h>

FOLLY_PUSH_WARNING
FOLLY_DETAIL_LITE_TUPLE_ADJUST_WARNINGS

// The `ext` namespace is not for end-users, but for library authors.
// Essentially, this is a public analog of `folly::detail`.  If you use it,
// expect to see churn on the API, and to be asked to help with changes.
namespace folly::ext {

// ## What are must-use-immediately types, and how should I handle them?
//
// Types satisfying the must-use-immediately concept follow these principles:
//  - The type is always immovable.
//  - APIs always take the type by-value, never by-reference.
//  - It typically supports the `unsafe_mover()` protocol, documented in detail
//    in the doc of the user-facing `must_use_immediately_unsafe_mover()`.
//
// Types usually need to become must-use-immediately because they contain
// references whose lifetime isn't safe enough for the context. Examples:
//
//  - In `folly/lang/bind/`, types often contain references to prvalues. Its
//    ad hoc reimplementation of must-use-immediately guarantees that refs
//    inside bindings remain valid. Future: Convert to `MustUseImmediately.h`.
//
//  - In `folly/coro`:
//      * Must-use-immediately coroutines (like `now_task`) let people
//        **safely** take args by-reference, which is otherwise unsafe.
//      * Types that contain refs with lifetime unknown, or too-short for safe
//        async usage will measure `safe_alias_of < closure_min_arg_safety`.
//      * Safe APIs (like pipelines and `then`) make types must-use-immediately
//        based on their `safe_alias` measurement, or the must-use-immediately
//        status of their parts.
//
// ## General protocol for creating must-use-immediately types
//
// To create a must-use-immediately type, either:
//
//   - If your type is movable and is OK to inherit from, then use
//     `wrap_must_use_immediately_t<YourBase>`. It automatically supplies an
//     `unsafe_mover()` implementation for the wrapped type.
//
//   - Derive `public must_use_immediately_crtp<YourType>`. In this case, you
//     will typically also want to implement the `unsafe_mover()` protocol. To
//     do so, read the ENTIRE doc of `must_use_immediately_unsafe_mover()`.
//     Especially: the notes on object slicing, and `noexcept` discipline. Then,
//     `curried_unsafe_mover_from_bases_and_members()` should be good enough.
//
// ## Handling must-use-immediately types in APIs
//
// APIs that work with must-use-immediately types must follow these rules:
//
// (1) Always take them by-value, never by-reference.
//
//     If your API must also support pass-by-reference for other types, then you
//     must bifurcate it on `must_use_immediately<T>` -- if `true`,
//     pass-by-value; if `false`, pass-by-reference.
//
// (2) NEVER expose non-static member functions for actions that consume the
//     must-use-immediately value -- `*this` is always by-reference.
//
//     INSTEAD: Use pass-by-value static member functions, or ADL CPOs.
//
// (3) Must-use-immediately types are immovable, but you may need to move them
//     internally in your API implementation.  For this, you can use the
//     callable from `must_use_immediately_unsafe_mover(std::move(t))`. For
//     example, you can pass `mover` into another coroutine, and then call
//     `mover()` to reconstitute the value.  The most typical pattern is to
//     forward a prvalue (by-value) argument by doing both on one line:
//     must_use_immediately_unsafe_mover(std::move(t))()
//
//     DANGER: DO NOT use this to move immovable types outside of your API,
//     where the lifetime safety was assured via pass-by-value (1) & (2).
//
// ## Case study: Must-use-immediately awaitables in `folly::coro`
//
// When `must_use_immediately_v<A> == true`, this indicates that the awaitable
// or semiawaitable `A` should be `co_await`ed in the full-expression that
// created it.  For an example, see `NowTask.h`.  Using immediate awaitables
// reduces the risk of lifetime bugs.
//
// To support rule (1), many coro APIs bifurcate on `must_use_immediately_v`.
// This is primarily done to support non-standard awaitables like `coro::Baton`,
// which must be awaited by-reference. The better solution would be to make
// `Baton::co_await()` return a must-use-immediately awaitable, and make most
// coro APIs take (semi)awaitables by-value.
//
// To support rule (2), we have been removing non-static member functions for
// actions that consume the (semi)awaitable.  For example:
//   - `task.scheduleOn()` was removed in favor of `co_withExecutor()`,
//   - `.start()` and `.semi()` are unsafe and omitted by `now_task`, etc.
//
// Caveat: If you encounter a public `folly::coro` API that does not yet handle
// must-use-immediately (semi)awaitables, and simply takes the input by `&&`,
// please fix it via one of these paths:
//   - If possible, switch it to only take (semi)awaitables by-value.
//   - If not, branch the API as in (1) above.
//   - Ask for help in the Coroutines group.
//
// ## Why do we need `folly_must_use_immediately_t` member type tags?
//
// Isn't immovability (and/or `unsafe_mover()`) enough?
//   - Not all must-use-immediately types have `unsafe_mover`.  In rare cases,
//     it is okay if the type cannot be moved.
//   - Some APIs, especially in `folly::coro`, must allow pass-by-reference. In
//     those cases, the API must bifurcate on `must_use_immediately_v` so that
//     use-immediately types are taken by value.
//   - Finally, the member-type-alias lets us add a (future) lint error whenever
//     an API takes a must-use-immediately arg, UNLESS it is marked with a
//     special `clang::annotate_type()` that excuses the bad behavior:
//     https://clang.llvm.org/docs/AttributeReference.html#annotate-type
//
// ## Why is the `unsafe_mover()` protocol required?
//
// Without prvalue-forwarding support in the language, it becomes impossible to
// write code like this:
//
//   auto compositeImmediate() { return combine(immediate1(), immediate2()); }
//
// The problem is that we cannot move `immediate1()` and `immediate2()` into any
// larger structure (they are immovable), but we also cannot take their
// references, since those become invalid upon `return`.
//
// In practice, it is essential to be able to write functions that compose
// must-use-immediately types, and we repeatedly found it necessary to add
// unsafe movers -- for coro (semi)awaitables, for bindings, and for pipelines.

namespace detail {

template <typename T>
using must_use_immediately_of_ =
    // User-defined types will only RARELY need to explicitly provide
    // `folly_must_use_immediately_t`.  Instead, prefer these:
    //  - Add via `wrap_must_use_immediately_t` or `must_use_immediately_crtp`.
    //  - Check its value via `must_use_immediately`.
    typename T::folly_must_use_immediately_t;

template <typename Void, typename T>
struct must_use_immediately_ {
  static_assert(
      require_sizeof<T>, "`must_use_immediately_t` on incomplete type");
  using type = std::false_type;
};

template <>
struct must_use_immediately_<void, void> {
  using type = std::false_type;
};

template <typename T>
struct must_use_immediately_<std::void_t<must_use_immediately_of_<T>>, T> {
  // This could do an "is `T` immovable" check, but the cost/benefit seems low.
  // Types that compose use-immediately types will automatically be immovable.
  //
  // So, an "is immovable" assert would only guard against a user disregarding
  // the `private`, not using `wrap_must_use_immediately_t` or
  // `must_use_immediately_crtp`, and instead WRONGLY adding this to the type:
  //   folly_must_use_immediately_t = std::true_type
  using type = must_use_immediately_of_<T>;
};

namespace must_use_immediately_tuple = folly::detail::lite_tuple;

} // namespace detail

template <typename T>
using must_use_immediately_t =
    typename detail::must_use_immediately_<void, T>::type;

template <typename T>
inline constexpr bool must_use_immediately_v = must_use_immediately_t<T>::value;

template <typename T>
concept must_use_immediately = must_use_immediately_v<T>;

// Types using `curried_unsafe_mover_t` need to offer a ctor with this passkey.
class curried_unsafe_mover_private_t {
 private:
  template <typename, typename...>
  friend class curried_unsafe_mover_t;
  curried_unsafe_mover_private_t() = default;
};

// Returned from `unsafe_mover` on typical must-use-immediately types.
//
// CONSTRUCTING THIS DIRECTLY IS A LAST RESORT --
//
// First, try inheriting `public wrap_must_use_immediately_t<YourClass>`, or
// `curried_unsafe_mover_from_bases_and_members()`. If these do not fit your
// usage, at least reimplement their `noexcept` and anti-slicing asserts.
//
// See `must_use_immediately_unsafe_mover()` for the docs.  In short, for an
// `Outer` that is must-use-immediately, this stores the constituent bases and
// members that are needed to reconstitute `Outer` via `operator()`.
//
// This contortion is necessary because prvalue semantics cannot be used in
// object initializers: "[copy elision only applies] when the object being
// initialized is known not to be a potentially-overlapping subobject".
//
// DANGER: Before returning this class from your `unsafe_mover()`, you must
// review "A note on object slicing" and make sure your usage isn't affected.
template <typename Outer, typename... Args>
class curried_unsafe_mover_t {
 private:
  detail::must_use_immediately_tuple::tuple<Args...> tup_;

 public:
  // AVOID CONSTRUCTING DIRECTLY -- see class doc.
  //
  // `Outer*` is just for type deduction and should be `nullptr`.
  //
  // IMPORTANT: If you change this to be conditionally `noexcept`, you must
  // update `curried_unsafe_mover_from_bases_and_members` to check that.
  explicit curried_unsafe_mover_t(Outer*, Args&&... args) noexcept
      : tup_{std::move(args)...} {
    // See must_use_immediately_unsafe_mover docblock
    static_assert((std::is_nothrow_move_constructible_v<Args> && ...));
  }
  // Why have both lvalue-ref and rvalue-ref variants?  Canonically, we'd want
  // this rvalue-ref qualified, but then if you try to curry more than one arg
  // via `std::move(mover).template get<I>()`, you will necessarily trigger
  // use-after-move linters.  So, we support this call style, too:
  //   std::move(mover.template get<I>())()
  template <size_t Idx>
  auto&& get() && noexcept {
    return std::move(detail::must_use_immediately_tuple::get<Idx>(tup_));
  }
  template <size_t Idx>
  auto& get() & noexcept {
    return detail::must_use_immediately_tuple::get<Idx>(tup_);
  }
  Outer operator()() && noexcept {
    curried_unsafe_mover_private_t priv;
    // See must_use_immediately_unsafe_mover docblock
    static_assert(noexcept(Outer{priv, std::move(*this)}));
    return Outer{priv, std::move(*this)};
  }
};

// Default impl of `must_use_immediately_unsafe_mover` for movable types
template <typename T>
struct default_unsafe_mover_t {
 private:
  T t_;

 public:
  explicit default_unsafe_mover_t(T&& t) noexcept : t_(std::move(t)) {
    // See `must_use_immediately_unsafe_mover` docblock
    static_assert(std::is_nothrow_move_constructible_v<T>);
  }
  T operator()() && noexcept { return std::move(t_); }
};

class must_use_immediately_private_t;

namespace detail {
template <typename T>
using must_use_immediately_detect_unsafe_mover_t = decltype(T::unsafe_mover(
    std::declval<must_use_immediately_private_t>(), FOLLY_DECLVAL(T&&)));
} // namespace detail

// CRITICAL: Read the docs on the definition below!
template <
    typename T,
    typename DetectRes = detected_or<
        default_unsafe_mover_t<T>,
        detail::must_use_immediately_detect_unsafe_mover_t,
        T>>
typename DetectRes::type must_use_immediately_unsafe_mover(T&& t) noexcept;

// Many must-use-immediately types will provide the static member
// `unsafe_mover(must_use_immediately_private_t, Me&&)`.  This passkey type
// prevents member name collisions.
class must_use_immediately_private_t {
 private:
  template <typename T, typename DetectRes>
  friend typename DetectRes::type must_use_immediately_unsafe_mover(
      T&& t) noexcept;
};

// Read the top-of-file doc, it's simpler to use `wrap_must_use_immediately_t`.
//
// Templated to ensure all bases are distinct in a tower-of-wrappers, like
// `TryAwaitable<now_task<T>>`.  Repeating a base would break the empty base
// optimization (EBO).
template <typename Derived>
struct must_use_immediately_crtp {
  using folly_must_use_immediately_t = std::true_type;

  must_use_immediately_crtp() = default;
  ~must_use_immediately_crtp() = default;

  // Avoid `folly::NonCopyableNonMovable` to avoid breaking EBO.
  must_use_immediately_crtp(must_use_immediately_crtp&&) = delete;
  must_use_immediately_crtp& operator=(must_use_immediately_crtp&&) = delete;
  must_use_immediately_crtp(const must_use_immediately_crtp&) = delete;
  must_use_immediately_crtp& operator=(const must_use_immediately_crtp&) =
      delete;

  template <safe_alias>
  using folly_private_safe_alias_t = safe_alias_constant<safe_alias::unsafe>;
};

// BEST FOR TYPICAL USAGE when inheriting `public must_use_immediately_crtp<>`.
//
// Creates a `curried_unsafe_mover_t` that decomposes an object into its base
// classes and member variables, allowing it to be safely moved and later
// reconstructed. This is the standard implementation pattern for
// `unsafe_mover()` methods in must-use-immediately types.
//
// Invariants:
//   - `Me` is never const, never an lvalue ref.
//   - `Me` is a standard-layout class.
//   - `PtrToMembers` are pointers to member variables of `Me` (or its bases).
//
// Pass argument in this order for the "forgot to curry" size heuristic to work:
//   - `Bases` go in the standard inheritance order: base-to-derived,
//     left-to-right.
//   - `PtrToMembers` within a sub-object should be in declaration order, across
//     sub-objects (uncommon!) in standard inheritance order.
//
// WATCH OUT: While construction logic would recommend moving them out in the
// reverse order of the above, but this is currently not done for lack of need.
//
// Design note: I considered eliding `default_unsafe_mover_t` when a base or
// member is movable.  This would make the mover type signature shorter, at the
// expense of complicating the uncurrying code.  Since it also doesn't reduce
// the number of move-copies, I decided against it.
template <
    // Mandatory template parameter: The class defining the `unsafe_mover` that
    // is calling this function. Detects object slicing.
    typename Caller,
    typename Me,
    typename... Bases,
    // Future: Unfortunately, MSVC2022 does not allow deducing the member-type
    // and object-type of `PtrToMembers` here, so we have to instantiate
    // `member_pointer_member_t` below.
    //
    // NB: We don't assert the object type of the pointer-to-member.  Although
    // typically it's just `Me...`, it can differ when packing a member from a
    // subobject that doesn't provide its own `unsafe_mover`.
    auto... PtrToMembers,
    typename RetVal = curried_unsafe_mover_t<
        Me,
        decltype(must_use_immediately_unsafe_mover(FOLLY_DECLVAL(Bases)))...,
        decltype(must_use_immediately_unsafe_mover(FOLLY_DECLVAL(
            member_pointer_member_t<decltype(PtrToMembers)>)))...>>
RetVal curried_unsafe_mover_from_bases_and_members(
    tag_t<Bases...>, vtag_t<PtrToMembers...>, Me&& me) noexcept {
  static_assert(!std::is_reference_v<Me>);
  static_assert((!std::is_reference_v<Bases> && ...));
  static_assert(std::is_convertible_v<Me&&, Caller&&>);
  // This slicing check is reliable and should remain mandatory. It fires if
  // `Me` is a derived class of `Caller` that added some data members or other
  // virtual inheritance. It's very likely that the caller needs to define its
  // own `unsafe_mover` to avoid object slicing bugs.
  static_assert(sizeof(Caller) == sizeof(Me));
  // A second heuristic to help detect when you forgot to curry some parts of
  // the class.  Since the curried mover is just a tuple, its size will
  // typically match the size of a "normal" class storing the same data in the
  // same order. The quotes around "normal" mean it's not an official notion --
  // this heuristic works for more interesting classes than "standard layout",
  // but it'll also definitely break if the class uses data packing features, or
  // virtual inheritance.
  //
  // The moment this breaks for a reasonable usage, we should add an opt-out.
  static_assert(sizeof(Me) == sizeof(RetVal));
  // NB: Assumes that the `curried_unsafe_mover_t` ctor is `noexcept`, and that
  // it already asserts that the inputs are nothrow-movable.
  return curried_unsafe_mover_t{
      (Me*)nullptr,
      [&]() -> decltype(auto) {
        static_assert(noexcept(
            must_use_immediately_unsafe_mover(static_cast<Bases&&>(me))));
        return must_use_immediately_unsafe_mover(static_cast<Bases&&>(me));
      }()...,
      [&]() -> decltype(auto) {
        auto&& member = static_cast<Me&&>(me).*PtrToMembers;
        static_assert(
            noexcept(must_use_immediately_unsafe_mover(std::move(member))));
        return must_use_immediately_unsafe_mover(std::move(member));
      }()...};
}

template <typename Inner>
class wrap_must_use_immediately_t
    : public Inner,
      public must_use_immediately_crtp<wrap_must_use_immediately_t<Inner>> {
 private:
  template <typename T>
  using my_curried_mover = curried_unsafe_mover_t<
      T,
      decltype(must_use_immediately_unsafe_mover(FOLLY_DECLVAL(Inner)))>;

 public:
  using Inner::Inner;

  // Required for base class disambiguation if a generic `Inner` already
  // defined `folly_must_use_immediately_t` (e.g. `TaskWrapper.h` sets it to
  // `false_type` for movable `Inner`).
  using typename must_use_immediately_crtp<
      wrap_must_use_immediately_t<Inner>>::folly_must_use_immediately_t;

  // Same disambiguation for `folly_private_safe_alias_t`. Must-use-immediately
  // types are unsafe since they contain references with unknown lifetimes.
  template <safe_alias>
  using folly_private_safe_alias_t = safe_alias_constant<safe_alias::unsafe>;

  // See the top-of-file docblock before defining your own `unsafe_mover`.
  //
  // `Me` is not a forwarding ref; the constraint will fail on lvalue refs.
  //
  // A more conservative design would be to require `is_same_v` here, which
  // would mean that deriving from a must-use-immediately type requires you
  // to provide a new `unsafe_mover` (typically a clone of the one in
  // `wrap_must_use_immediately_t`).
  //
  // However, reusing the `unsafe_mover` of a base-class is fairly
  // low-risk, since we do a simple `sizeof` check for object slicing here.
  // Undetected problems could only arise if the derived classes adds
  // mandatory side effects in its destructor / move / assignment.
  template <typename Me>
    requires std::is_base_of_v<Inner, Me>
  static my_curried_mover<Me> unsafe_mover(
      must_use_immediately_private_t, Me&& me) noexcept {
    return curried_unsafe_mover_from_bases_and_members<
        wrap_must_use_immediately_t>(
        tag<Inner>, vtag</*no members*/>, static_cast<Me&&>(me));
  }
  // Matches the constraint logic in our `unsafe_mover`
  template <typename DerivedFromMe>
    requires std::is_base_of_v<wrap_must_use_immediately_t, DerivedFromMe>
  explicit wrap_must_use_immediately_t(
      curried_unsafe_mover_private_t,
      my_curried_mover<DerivedFromMe>&& mover) noexcept(noexcept(Inner{
      std::move(mover).template get<0>()()}))
      : Inner{std::move(mover).template get<0>()()} {}
};

// When a function takes a must-use-immediately type by-value, it may use the
// `must_use_immediately_unsafe_mover()` protocol to move it internally.  To
// retain lifetime-safety, the function MUST NOT allow the value be used outside
// of its own lifetime. Rationale: the must-use-immediately args may contain
// references that become invalid at the end of the fn call's full-expression.
//
// Wraps `T::unsafe_mover(must_use_immediately_private_t, T&&)` for immovable
// types that implement it. For movable types that don't support the protocol,
// falls back to `default_unsafe_mover_t`.
//
// ## Implementing `T::unsafe_mover(must_use_immediately_private_t, T&&)`
//
// Required semantics:
//
//  - `unsafe_mover` is a destructive operation on an immovable type.  Note:
//    `T&&` is an rvalue ref, not a forwarding one.  Do NOT support lval refs.
//  - It takes ownership of the internals of `T`, leaving it "moved out".
//  - It returns a mover value (never a reference), whose `operator() &&` is a
//    single-use operation returning a prvalue equivalent to the original `T`.
//  - Both construction and invocation of the returned mover are `noexcept`.
//
// Written without constraints, `Base::unsafe_mover()` will also be invocable on
// derived instances. This is occasionally desirable, but comes with a
// significant risk of object-slicing bugs (next section), so implementations
// should lock down the type of `T` as much as practical, via patterns like:
//
//   template <typename U, std::enable_if_t<std::is_same_v<U, T>, int> = 0>
//   static auto unsafe_mover(must_use_immediately_private_t, U&&);
//
// ### A note on object slicing when inheriting from must-use-immediately types
//
// All `unsafe_mover` implementations work by moving out the constituent parts
// of the immovable type. Since parts can be must-use-immediately / immovable,
// we'll usually call `must_use_immediately_unsafe_mover()` on each one.
//
// An object slicing problem will occur whenever `Base::unsafe_mover` is called
// on a `Derived` class with one of these properties:
//
// (a) It adds a new member variable, or new base with member variables. Then,
//     `Base::unsafe_mover` wouldn't know about the new data and discard it.
//
// (b) It adds new side effects to destruction, move, copy, or assignment. Then,
//     the `unsafe_mover()`'s wrapping/unwrapping will cause the custom logic to
//     run at an unexpected time.
//
// Such `Derived` types MUST customize `unsafe_mover` to return a custom mover
// that accounts for the slicing-unfriendly behaviors (a) or (b).
//
// Some slicing-prevention strategies:
//   - Prefer composition (`Inner inner_;`) over inheritance.
//   - Prevent `Base::unsafe_mover()` from being used with derived classes via
//     constraints (example above), by marking the class `final`, or both.
//   - If you must allow derived classes, then either use
//     `curried_unsafe_mover_from_bases_and_members()`, which implements a
//     `sizeof()`-based slicing detection heuristic, or (last resort) review its
//     comments & implement your own.
//   - Test, test, test: Whenever you derive from a must-use-immediately class,
//     round-trip it via an unsafe mover and confirm it works as desired.
//
// There is no silver bullet. For example, comparing `sizeof()` can have
// false-positives (e.g. adding virtual inheritance) and false-negatives (side
// effects in dtor / move / etc). When inheriting from must-use-immediately,
// request thoughtful code review, and leave comments pointing at this doc for
// the next person to touch your code.
//
// ## A note on `noexcept` discipline
//
// This implementation `static_assert`s that constructing **and** using a mover
// is `noexcept`. Internally, movers implementations must not neglect to test
// their own operations via `noexcept()` expressions.
//
// In the future this current API could be relaxed to `noexcept(noexcept(...))`,
// but none of the current use-cases SHOULD have throwing move ctors
// (`folly::bind` bindings, `folly::coro` awaitables, `folly::coro` pipelines).
//
// In the unlikely event we make this change, we'd have to add a new
// `static_assert` in every place that uses `must_use_immediately_unsafe_mover`
// to make sure it's still `noexcept` in that usage ...  or just fix it up.
//
// So, this is an expensive, breaking change.  But, it is probably not worth it
// to forward-engineer this, and all its current consumers, to use
// `noexcept(noexcept())` for the sake of a use-case that may never come.
//
// ### Why is `unsafe_mover` a static method?
//
// This has two outcomes:
//   - Each `unsafe_mover` can explicitly decide whether to accept subclasses.
//     As discussed above, this helps types manage object slicing risk.
//   - Unimportantly, writing `T::unsafe_mover` **below** is a cheap syntax-only
//     way of ensuring that `T&&` is NOT a forwarding ref, but an rvalue ref.
template <typename T, typename DetectRes>
// DANGER: Passing by `&&` can violate the use-immediately restriction!  The
// caller MUST have taken `t` by-value to make it OK to call this.
//
// NB: `T` is not a forwarding ref, see `static_assert` below.
typename DetectRes::type must_use_immediately_unsafe_mover(T&& t) noexcept {
  static_assert(!std::is_reference_v<T>);
  static_assert(noexcept(FOLLY_DECLVAL(typename DetectRes::type&&)()));
  if constexpr (DetectRes::value_t::value) {
    must_use_immediately_private_t priv;
    static_assert(noexcept(T::unsafe_mover(priv, static_cast<T&&>(t))));
    static_assert(
        std::is_same_v<
            T,
            decltype(T::unsafe_mover(priv, static_cast<T&&>(t))())>,
        "The result of unsafely moving a T must be exactly T to ensure that we "
        "satisfy the preconditions for mandatory copy elision, which eliminates "
        "the possibility of exceptions during conversion");
    return T::unsafe_mover(priv, static_cast<T&&>(t));
  } else {
    static_assert(
        std::is_nothrow_constructible_v<default_unsafe_mover_t<T>, T&&>);
    return default_unsafe_mover_t<T>{static_cast<T&&>(t)};
  }
}

} // namespace folly::ext

FOLLY_POP_WARNING
