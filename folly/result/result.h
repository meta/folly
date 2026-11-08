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

#include <folly/ExceptionWrapper.h>
#include <folly/Expected.h>
#include <folly/OperationCancelled.h>
#include <folly/Portability.h> // FOLLY_HAS_RESULT
#include <folly/Utility.h> // FOLLY_DECLVAL
#include <folly/lang/Align.h> // for `hardware_constructive_interference_size`
#include <folly/lang/RValueReferenceWrapper.h>
#include <folly/portability/GTestProd.h>
#include <folly/result/rich_exception_ptr.h>

/// Read the full docs in `result.md`!
///
/// `result<T>` resembles `std::variant<T, std::exception_ptr, stopped_result>`,
/// but is cheaper, and targets "never empty" semantics in C++23.  Its intended
/// use-case is a "better `Try<T>`", both in sync & `folly::coro` code:
///
///   - No "empty state" wart -- all states of `result` have a clear meaning,
///     as far as control flow is concerned.
///
///   - Easy exception checks:
///       if (auto ex = folly::get_exception<Ex>(res)) {
///         // Handle `ex`, quacks like `const Ex*`
///       }
///
///   - User-friendly constructors & conversions -- you can write
///     `result<T>`-returning functions as-if they returned `T`, while
///     returning `error_or_stopped{YourException{...}}` on error.
///
///   - Can store & and && references.  Think of them as syntax sugar for
///     `std::reference_wrapper` and `folly::rvalue_reference_wrapper`.
///
///        #include <folly/result/coro.h>
///        struct FancyIntMap {
///          int n;
///          result<int&> at(int i) {
///            if (n + i == 42) { return std::ref(n); }
///            return error_or_stopped{std::out_of_range{"FancyIntMap"}};
///          }
///        };
///        FancyIntMap m{.n = 12};
///        int& n1 = co_await or_unwind(m.at(30)); // points at 12
///        result<int&> rn2 = m.at(20); // has error
///        // On error, copies and propagates `exception_ptr` to the parent.
///        // When errors are hot, `std::move()` is appropriate.
///        co_return n1 + co_await or_unwind(rn2);
///
///     Key things to remember:
///       - `result<V&&>` is "use-once" -- and it must be r-value qualified to
///         access the reference inside.
///       - `const result<V&>` only gives `const V&` access to the contents.
///         It should be rare that you need the opposite behavior.  If you do,
///         use `result<std::reference_wrapper<V>>` or `result<V*>`.
///
///   - `has_stopped()` & `stopped_result` to nudge `folly` to the C++26 idea
///     that cancellation is NOT an error, see https://wg21.link/P1677 & P2300.
///
///   - Easy short-circuiting of "error" / "stopped" status to the caller:
///     * In both `result<T>` coroutines and `folly::coro` coroutines:
///       - `co_await or_unwind(resultFn())` returns `T&&` from a `result<T>`,
///         or propagates error/stopped to the parent. Same for:
///           * `co_await or_unwind(std::move(res))`
///           * `co_await or_unwind(folly::copy(res))`
///       - `co_await or_unwind(res) -> T&`. Copies `exception_ptr` on error.
///       - `co_await or_unwind(std::as_const(res)) -> const T&`
///       WARNING: `auto&& ref = co_await or_unwind(rvalueFn())` dangles; search
///       `result.md` for "LLVM issue #177023".  Safe: `auto val = ...`
///       - `co_await stopped_result` or `error_or_stopped{YourErr{}}` to
///         end the coroutine with an error without throwing.
///     * In `folly::coro` coroutines:
///       - `co_await value_or_error_or_stopped(x())` makes `result<X>`, does
///         not throw.
///       - `co_yield co_result(std::move(res))` completes with a `result<T>`.
///     * While you should strongly prefer to write `result<T>` coroutines,
///       propagation in non-coroutine `result<T>` functions is also easy:
///         if (auto* value = res.get_pointer()) {
///           return *value;
///         }
///         return std::move(res).error_or_stopped();
///
///   - `result` is mainly used for return values -- implying single ownership.
///     It is copyable (when `T` is), but moves save ~7ns on the error path.
///
/// Note: Unlike `Try`, `error_or_stopped` (and thus `result<T>` in an
/// error-or-stopped state) will `std::terminate` in debug builds if you attempt
/// to construct it, or access it while it contains either of:
///
///   - `OperationCancelled` -- the header explains why user code should not
///     use that exception.  Instead, store `stopped_result`, and use
///     `has_stopped()` to check for its presence.
///
///   - An empty `std::exception_ptr`.  For prior art, consider that
///     `exception_wrapper::throw_exception` unconditionally calls
///     `std::terminate` when the wrapper is empty.  By explicitly specifying
///     this as out-of-contract, and validating eagerly, we reserve this
///     representation to potentially mean something else in the future.

#if FOLLY_HAS_RESULT

namespace folly {

struct OperationCancelled;

namespace detail {

// In order to give `result` a stronger contract, debug builds prevent `result`
// and `error_or_stopped` from ingesting empty `std::exception_ptr`s, and ones
// with `OperationCancelled`.
//
// In prod, neither check is done since the legacy behaviors are "okay"-ish:
//   - Empty `std::exception_ptr`s, while nonsensical in the context of
//     `result`, are safe to use unless you call `throw_exception()`.  And,
//     unfortunately, `co_yield co_error(exception_wrapper{})` compiles.
//   - As of 2026, erroring with `OperationCancelled` is the implementation of
//     `co_yield co_stopped_may_throw`, and some code paths actually rely on
//     this, often erroneously (see `coro/Retry.h`).  So, even as we work to
//     reduce reliance on this in anticipation of C++26 "stopped" semantics,
//     for the foreseeable future it will "sort of work".
void fatal_if_eptr_empty_or_stopped(const std::exception_ptr&);
inline void dfatal_if_eptr_empty_or_stopped(const std::exception_ptr& eptr) {
  // This can be hot in production code (usage similar to `co_awaitTry`).  So,
  // we choose to omit `if (RTTI-test-for-stopped) { log(); }` from opt builds.
  if constexpr (kIsDebug) {
    fatal_if_eptr_empty_or_stopped(eptr);
  }
}

// Do NOT use outside of `folly`, the APIs this gates WILL change.
struct result_private_t {};

template <typename, typename>
class result_crtp; // Forward decl for `error_or_stopped` friend

} // namespace detail

// Place this into `result` or `error_or_stopped` to signal that a work-tree
// was stopped (aka cancelled).  You can also `co_await stopped_result` from
// `result` coroutines.
struct stopped_result_t {};
inline constexpr stopped_result_t stopped_result;

template <typename, typename, auto...>
class immortal_rich_error_t;

// NB: Copying `error_or_stopped` is ~7ns due to `std::exception_ptr` atomics.
// Unlike `result`, it is implicitly copyable, because:
//   - Common usage involves only rvalues, so the risk of perf bugs is low.
//   - `folly::Expected` assumes that the error type is copyable, and it's
//     too convenient an implementation not to use.
class [[nodiscard]] error_or_stopped {
 private:
  rich_exception_ptr rep_;

  error_or_stopped(std::in_place_t, std::exception_ptr&& eptr) noexcept
      : rep_{rich_exception_ptr::from_exception_ptr_slow(std::move(eptr))} {}

  // Private: for `result_crtp` and `result` to construct "has value" state
  template <typename, typename>
  friend class detail::result_crtp;
  template <typename>
  friend class result;
  explicit error_or_stopped(
      vtag_t<detail::private_rich_exception_ptr_sigil::RESULT_HAS_VALUE>
          sigil) noexcept
      : rep_{sigil} {}
  [[nodiscard]] constexpr bool is_result_has_value() const noexcept {
    return rep_.has_sigil<
        detail::private_rich_exception_ptr_sigil::RESULT_HAS_VALUE>();
  }

 public:
  /// Future: Fine to make implicit if a good use-case arises.
  explicit error_or_stopped(stopped_result_t) : rep_{OperationCancelled{}} {}
  error_or_stopped& operator=(stopped_result_t) {
    rep_ = rich_exception_ptr{OperationCancelled{}};
    return *this;
  }

  /// Use this ctor to report errors from `result` coroutines & functions:
  ///   co_await error_or_stopped{YourError{...}};
  ///
  /// Hot error paths should consider the `immortal_rich_error_t` ctor instead.
  ///
  /// Design note: We do NOT want most users to construct `error_or_stopped`
  /// from type-erased `std::exception_ptr` or `folly::exception_wrapper`,
  /// because that would block RTTI-avoidance optimizations for `result` code.
  explicit error_or_stopped(std::derived_from<std::exception> auto ex)
      : rep_(std::move(ex)) {
    static_assert(
        !std::is_same_v<decltype(ex), OperationCancelled>,
        // The reasons for this are discussed in `folly/OperationCancelled.h`.
        "Do not use `OperationCancelled` in new user code. Instead, construct "
        "your `result` or `error_or_stopped` via `stopped_result`");
  }

  /// Immortal rich errors are MUCH cheaper to instantiate than dynamic
  /// exceptions.  This does NOT allocate a `std::exception_ptr` or perform
  /// atomic refcount ops.
  ///
  /// Usage for a `YourErr` taking a single `rich_msg` constructor argument:
  ///    error_or_stopped{immortal_error<YourErr, "msg"_litv>}
  ///
  /// PS These are also usable in `constexpr` code, although the current header
  /// will need some more `constexpr` annotation to take advantage of this.
  template <typename T, auto... Args>
  explicit error_or_stopped(
      const immortal_rich_error_t<rich_exception_ptr, T, Args...>& err)
      : rep_{err.ptr()} {}

  // PRIVATE, use `stopped_nothrow` (future) instead.
  explicit error_or_stopped(detail::StoppedNoThrow s) : rep_(s) {}

  [[nodiscard]] bool has_stopped() const {
    return bool{::folly::get_exception<OperationCancelled>(rep_)};
  }

  // Implement the `folly::get_exception<Ex>(res)` protocol
  template <typename Ex>
  rich_ptr_to_underlying_error<const Ex> get_exception(
      get_exception_tag_t) const noexcept {
    static_assert( // Note: `OperationCancelled` is final
        !std::is_same_v<const OperationCancelled, const Ex>,
        "Test results for cancellation via `has_stopped()`");
    return folly::get_exception<Ex>(rep_);
  }
  template <typename Ex>
  rich_ptr_to_underlying_error<Ex> get_mutable_exception(
      get_exception_tag_t) noexcept {
    static_assert( // Note: `OperationCancelled` is final
        !std::is_same_v<const OperationCancelled, const Ex>,
        "Test results for cancellation via `has_stopped()`");
    return folly::get_mutable_exception<Ex>(rep_);
  }

  // AVOID -- throwing costs upwards of 1usec.
  //
  // NB: Throwing empty eptr currently terminates even in non-debug builds,
  // following `exception_wrapper`.  Our `dfatal_if_eptr_empty_or_stopped`
  // checks are intended to make such problems less likely, but ...  if
  // production reliability issues trace back to this decision, it could be
  // made to throw a private sigil instead.
  //
  // While user code should not intentionally rethrow `OperationCancelled` on
  // this path, we do NOT dfatal on rethrowing `OperationCancelled` here, in
  // contrast to `to_exception_ptr_slow()`. The motivation is:
  //
  //   - `result` is intended to become the next internal representation
  //     for `folly::coro` task results.
  //
  //   - `value_or_throw()` is the only reasonable bridge from coro code into
  //     non-coro code (see `blocking_wait`, `SemiFuture` conversions, etc).
  //     On exiting coro / result code, there is no real alternative to
  //     propagating cancellation as an exception.
  //
  //   - Internally, `value_or_throw()` uses `throw_exception()`.  And it would
  //     be incoherent for one, but not the other, to be OK rethrowing
  //     `OperationCancelled`.
  [[noreturn]] void throw_exception() const { rep_.throw_exception(); }

  /// AVOID.  Use `error_or_stopped(YourException{...})` if at all possible.
  /// Add a `std::in_place_type_t<Ex>` constructor if needed.
  ///
  /// Provided for compatibility with existing code.  It has several downsides
  /// for `result`-first code:
  ///   - It is a debug-fatal invariant violation to pass in an `exception_ptr`
  ///     that is empty or has `OperationCancelled`.
  ///     See the `dfatal_if_eptr_empty_or_stopped` doc.
  ///   - Not knowing the static exception type blocks optimizations that can
  ///     otherwise help avoid RTTI on error paths.
  static error_or_stopped from_exception_ptr_slow(std::exception_ptr eptr) {
    detail::dfatal_if_eptr_empty_or_stopped(eptr);
    return error_or_stopped{std::in_place, std::move(eptr)};
  }

  /// AVOID. Use `folly::get_exception<Ex>(r)` to check for specific exceptions.
  /// It may be OK to add more specific accessors to `error_or_stopped`, see
  /// `throw_exception()` for an example.
  ///
  /// INVARIANT: Ensure `!has_stopped()`, or you will see a debug-fatal.
  ///
  /// See `from_exception_ptr_slow` for the downsides and the rationale.
  [[nodiscard]] std::exception_ptr to_exception_ptr_slow() && {
    auto eptr = std::move(rep_).to_exception_ptr_slow();
    detail::dfatal_if_eptr_empty_or_stopped(eptr);
    return detail::extract_exception_ptr(std::move(eptr));
  }

  /// AVOID.  Most code should use `result` coros, which catch most exceptions
  /// automatically.  Or, for a stronger guarantee, see `result_catch_all`.
  static error_or_stopped from_current_exception() {
    // Something was already thrown, and the user likely wants a result, so
    // it's appropriate to accept even `OperationCancelled` here.
    return {std::in_place, current_exception()};
  }

  friend inline bool operator==(
      const error_or_stopped& lhs, const error_or_stopped& rhs) {
    return lhs.rep_ == rhs.rep_;
  }

  // DO NOT USE these "legacy" functions outside of `folly` internals. Instead:
  //   - `error_or_stopped(YourException{...})` whenever you statically know
  //     the exception type (feel free to add `std::in_place_type_t` support).
  //   - `error_or_stopped::from_exception_ptr_slow()` only when you MUST pay
  //     for RTTI, such as "thrown exceptions".
  //
  // See `OperationCancelled.h` for how to handle cancellation.  In short: use
  // `get_exception<MyErr>(res)` or `has_stopped()`.
  //
  // These internal-only functions let the `folly::coro` implementation ingest
  // `std::exception_ptr`s containing `OperationCancelled` made via
  // `folly::coro::co_stopped_may_throw`, without incurring the 20-80ns+ cost
  // of eagerly testing whether it contains `OperationCancelled`.
  static error_or_stopped make_legacy_error_or_cancellation_slow(
      detail::result_private_t, exception_wrapper ew) {
    return {std::in_place, std::move(ew).exception_ptr()};
  }
  exception_wrapper get_legacy_error_or_cancellation_slow(
      detail::result_private_t) && {
    return exception_wrapper{std::move(rep_).to_exception_ptr_slow()};
  }

  // IMPORTANT: We do NOT want to provide general by-reference access to the
  // `rich_exception_ptr` because that would e.g. put in jeopardy our ability
  // to do `future_epitaph_in_place.md`.
  //
  // In particular, it is an invariant violation to call `release_...` and
  // use the resulting reference for anything other than:
  //   - moving out the value (if you need a copy, add an explicit
  //     `copy_rich_exception_ptr`)
  //   - doing nothing (i.e. deciding NOT to move the value)
  // You are not to call `const` or mutable accessors on the resulting REP.  If
  // you need some such form of access, you should likely extend this API.
  rich_exception_ptr&& release_rich_exception_ptr() && {
    return std::move(rep_);
  }

  // Implementation of `fmt::format` and `operator<<(ostream&)`.
  void format_to(fmt::appender out) const { rep_.format_to(out); }
};
static_assert(
    detail::rich_exception_ptr_packed_storage::is_supported
        ? sizeof(error_or_stopped) == sizeof(std::exception_ptr)
        : sizeof(error_or_stopped) ==
            sizeof(std::exception_ptr) + sizeof(void*));

// Backwards-compatible alias
using non_value_result = error_or_stopped;

template <typename T = void>
class result;

namespace detail {

template <typename>
class result_promise_return;
template <typename, typename = void>
struct result_promise; // Build error? #include <folly/result/coro.h>

// Future: To mitigate the risk of `bad_alloc` at runtime, these singletons
// should be eagerly instantiated at program start.  One way is to have a
// `shouldEagerInit` singleton in charge of this, and tell the users to do
// this on startup:
//   folly::SingletonVault::singleton()->doEagerInit();
const error_or_stopped& dfatal_get_bad_result_access_error();

template <typename T>
using result_ref_wrap = conditional_t< // Reused by `result_generator`
    std::is_rvalue_reference_v<T>,
    rvalue_reference_wrapper<std::remove_reference_t<T>>,
    conditional_t<
        std::is_lvalue_reference_v<T>,
        std::reference_wrapper<std::remove_reference_t<T>>,
        T>>;

// Shared implementation for `T` non-`void` and `void`
template <typename Derived, typename T>
class result_crtp {
  static_assert(
      !std::is_same_v<class error_or_stopped, std::remove_cvref_t<T>>);
  static_assert(!std::is_same_v<stopped_result_t, std::remove_cvref_t<T>>);

 public:
  using value_type = T; // NB: can be a reference

 protected:
  using storage_type = detail::result_ref_wrap<lift_unit_t<T>>;
  static_assert(!std::is_reference_v<storage_type>);

  template <typename>
  friend class folly::result; // Cross-instantiation uses `eos_`

  template <typename, typename>
  friend class result_or_unwind_crtp; // `await_suspend` uses `eos_`

  template <typename>
  friend struct result_promise_base; // `unhandled_exception` uses `eos_`

  // Only non-void derived classes have `value_`.
  // `value_` is initialized iff `eos_.is_result_has_value()` is true.
  class error_or_stopped eos_;

  result_crtp(result_crtp&& that) noexcept : eos_{std::move(that.eos_)} {}
  result_crtp& operator=(result_crtp&&) = delete; // See derived classes

  template <typename ResultT>
  static Derived rewrapping_result_convert(ResultT&& rt) {
    static_assert(is_instantiation_of_v<result, std::remove_cvref_t<ResultT>>);
    if (rt.has_value()) [[likely]] {
      // Implicitly convert `ResultT::value_type` to `Derived`.
      return {std::forward<ResultT>(rt).value_or_throw()};
    }
    // `Derived` lets the rewrapping conversion copy an error-or-stopped state
    return Derived{std::forward<ResultT>(rt).error_or_stopped()};
  }

  // Derived classes use `has_value_sigil_t` to construct value-state results.
  //
  // WARNING: The promise return object ALSO calls this -- it sets `eos_` to
  // "has value" sigil, leaving `value_` uninitialized.  `return_value` uses
  // placement `new`; `unhandled_exception` directly sets `eos_`.  All uses
  // bypass assignment, which would try to destroy uninitialized `value_`.
  using has_value_sigil_t =
      vtag_t<private_rich_exception_ptr_sigil::RESULT_HAS_VALUE>;
  explicit result_crtp(has_value_sigil_t s) : eos_{s} {}

  void throw_if_no_value() const {
    if (!eos_.is_result_has_value()) [[unlikely]] {
      eos_.throw_exception();
    }
  }

  explicit result_crtp(class error_or_stopped&& eos) : eos_{std::move(eos)} {}
  explicit result_crtp(const class error_or_stopped& eos) : eos_{eos} {}

 public:
  ~result_crtp() = default;

  /********* Construction & assignment for `T` `void` and non-`void` **********/

  /// `result<T>` is copyable iff `T` is copyable.
  ///
  /// ## Copies are restricted when `T` is a reference
  ///
  /// `const result<V&>` only gives access to `const V&`, for reasons discussed
  /// in `result.md` under "Store references...".  Standard copy semantics
  /// allows creating a new `result<V&>` from `const result<V&>&`.  But, this
  /// would present a const-safety problem -- since `result<V&>` gives access
  /// to a mutable `V&`. To fix this, copying `result<V&>` is ONLY allowed:
  ///   - from `result<V&>&`, since you already have mutable access
  ///   - from `const result<const V&>&`, since the inner `const` is not
  ///     lost during the copy.
  ///
  /// See `docs/design_notes.md` for details.

  [[deprecated("result<T> is copyable; use folly::copy() for explicit copies")]]
  Derived copy()
    requires(
        !std::is_rvalue_reference_v<T> &&
        (std::is_void_v<T> || std::is_copy_constructible_v<T>))
  {
    return static_cast<Derived&>(*this);
  }
  [[deprecated("result<T> is copyable; use folly::copy() for explicit copies")]]
  Derived copy() const
    requires(
        (!std::is_reference_v<T> ||
         std::is_const_v<std::remove_reference_t<T>>) &&
        (std::is_void_v<T> || std::is_copy_constructible_v<T>))
  {
    return static_cast<const Derived&>(*this);
  }
  // Leaf classes provide copy ctors with deep-const constraints for refs.
  result_crtp(const result_crtp&) = default;
  result_crtp& operator=(const result_crtp&) = delete;

  /***************** Accessors for `T` `void` and non-`void` ******************/

  [[nodiscard]] bool has_value() const { return eos_.is_result_has_value(); }
  // Also see `has_stopped()` below!

  /// Error-or-stopped access should be used SPARINGLY!
  ///
  /// Normally, you would:
  ///   - `folly::get_exception<Ex>(res)` to test for a specific error.
  ///   - `res.has_stopped()` to test for cancellation.
  ///   - `co_await or_unwind(std::move(res))` to propagate unhandled
  ///     error/cancellation in `result` sync coroutines, or in `folly::coro`
  ///     async task coroutines.
  ///
  /// Design notes:
  ///
  /// There is no mutable `&` overload so that we can return singleton
  /// invariant-violation exceptions for `dfatal_..._error()` in opt builds:
  ///   - Calling `error_or_stopped()` when `has_value() == true` -- UB in
  ///     `std::expected`
  /// With folly-internal optimizations (see `extract_exception_ptr`), moving
  /// `std::exception_ptr` takes 0.5ns, vs ~7ns for a copy.
  ///
  /// If there is a good use-case for mutating the error-or-stopped state inside
  /// `result`, we could offer `set_error_or_stopped()` with different
  /// semantics.
  class error_or_stopped error_or_stopped() && noexcept {
    if (!eos_.is_result_has_value()) [[likely]] {
      return std::move(eos_);
    }
    return detail::dfatal_get_bad_result_access_error();
  }
  // noexcept: std::exception_ptr copy is non-throwing on all major platforms
  // (atomic refcount increment via Itanium ABI or MSVC shared_ptr-like impl).
  const class error_or_stopped& error_or_stopped() const& noexcept {
    if (!eos_.is_result_has_value()) [[likely]] {
      return eos_;
    }
    return detail::dfatal_get_bad_result_access_error();
  }

  // Backwards-compatible aliases
  class error_or_stopped non_value() && noexcept {
    return std::move(*this).error_or_stopped();
  }
  const class error_or_stopped& non_value() const& noexcept {
    return error_or_stopped();
  }

  // Syntax sugar to help end-users avoid `error_or_stopped()`.
  [[nodiscard]] bool has_stopped() const {
    return !has_value() && error_or_stopped().has_stopped();
  }

  /********************************* Protocols ********************************/

  // `result` is a short-circuiting coroutine.
  using promise_type = detail::result_promise<T>;

  // Implement the `folly::get_exception<Ex>(res)` protocol
  template <typename Ex>
  rich_ptr_to_underlying_error<const Ex> get_exception(
      get_exception_tag_t) const noexcept {
    if (eos_.is_result_has_value()) {
      return rich_ptr_to_underlying_error<const Ex>{nullptr};
    }
    return folly::get_exception<Ex>(eos_);
  }
  template <typename Ex>
  rich_ptr_to_underlying_error<Ex> get_mutable_exception(
      get_exception_tag_t) noexcept {
    if (eos_.is_result_has_value()) {
      return rich_ptr_to_underlying_error<Ex>{nullptr};
    }
    return folly::get_mutable_exception<Ex>(eos_);
  }
};

} // namespace detail

// The default specialization is non-`void` (but `result<>` defaults to `void`)
template <typename T>
class [[nodiscard]] [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE]]
result final : public detail::result_crtp<result<T>, T> {
 private:
  template <typename, typename>
  friend class detail::result_crtp; // conversion ctors access `value_`, `eos_`
  template <typename>
  friend class result; // conversion ctors access `value_`, `eos_`
  friend struct detail::result_promise<T>;
  friend class detail::result_promise_return<T>;

  using base = typename detail::result_crtp<result<T>, T>;
  // For `T` non-`void`, we store either `T` or a ref wrapper.
  using ref_wrapped_t = typename base::storage_type;
  // `true` for `result<V&>` where `V` is non-const.  These use deep-const copy
  // semantics: only copyable from a mutable source (see `design_notes.md`).
  static constexpr bool is_mutable_lref_v = std::is_lvalue_reference_v<T> &&
      !std::is_const_v<std::remove_reference_t<T>>;

  // Value storage in non-`void` result (not in CRTP base).
  // Use union to avoid requiring default-constructibility.
  union {
    ref_wrapped_t value_;
  };

  template <typename Self>
  result& copy_assign_from(Self& that) {
    if (this != &that) [[likely]] {
      const bool this_v = this->eos_.is_result_has_value();
      const bool that_v = that.eos_.is_result_has_value();
      if (this_v && that_v) {
        value_ = that.value_;
      } else if (this_v) {
        value_.~ref_wrapped_t();
        this->eos_ = that.eos_;
      } else if (that_v) {
        // Construct first; if it throws, we stay in error state.
        new (&value_) ref_wrapped_t{that.value_};
        this->eos_ = that.eos_;
      } else {
        this->eos_ = that.eos_;
      }
    }
    return *this;
  }

 public:
  ~result() {
    if (this->eos_.is_result_has_value()) {
      value_.~ref_wrapped_t();
    }
  }

  /// Not default-constructible yet, since the utility is debatable.  If we
  /// were to later make `result` default-constructible, it should follow
  /// `std::expected` semantics, as below.  As of now, there are only a couple
  /// of tests in `result_test.cpp` marked "not default-constructible".

  /*
  /// Default-construct as `std::expected` would, and unlike `folly::Expected`
  result() noexcept(noexcept(ref_wrapped_t{}))
    requires std::is_default_constructible_v<ref_wrapped_t>
      : base{typename base::has_value_sigil_t{}} {
    new (&value_) ref_wrapped_t{};
  }

  TEST(result, defaultCtor) {
    result<> mVoid;
    EXPECT_TRUE(mVoid.has_value());
    result<int> mInt;
    EXPECT_EQ(0, mInt.value_or_throw());
  }
  */

  /// Movable iff `T` is
  result(result&& that) // No `.value_` in decl, see FIXME(clang-ice)
      noexcept(std::is_nothrow_move_constructible_v<ref_wrapped_t>)
    requires std::is_move_constructible_v<ref_wrapped_t>
      : base{std::move(that)} {
    if (this->eos_.is_result_has_value()) {
      new (&value_) ref_wrapped_t{std::move(that.value_)};
    }
  }
  result& operator=(result&& that) // No `.value_` in decl, see FIXME(clang-ice)
      noexcept(
          std::is_nothrow_move_assignable_v<ref_wrapped_t> &&
          std::is_nothrow_move_constructible_v<ref_wrapped_t>)
    requires(
        std::is_move_assignable_v<ref_wrapped_t> &&
        std::is_move_constructible_v<ref_wrapped_t>)
  {
    if (this != &that) [[likely]] {
      const bool this_has_val = this->eos_.is_result_has_value();
      const bool that_has_val = that.eos_.is_result_has_value();

      if (this_has_val && that_has_val) {
        // Value to value: use move assignment (can throw, stays in value state)
        value_ = std::move(that.value_);
      } else if (this_has_val && !that_has_val) {
        // Value to error: destroy value, move error (nothrow)
        value_.~ref_wrapped_t();
        this->eos_ = std::move(that.eos_);
      } else if (!this_has_val && that_has_val) {
        // Construct first; if it throws, we stay in error state (no valueless).
        new (&value_) ref_wrapped_t{std::move(that.value_)};
        this->eos_ = std::move(that.eos_);
      } else {
        // Error to error: just move error (nothrow)
        this->eos_ = std::move(that.eos_);
      }
    }
    return *this;
  }

  /// Copyable iff `T` is, except:
  ///  - `result<V&&>` is move-only.
  ///  - For `result<V&>` with `V` non-const, can only copy only from a mutable
  ///    source -- this prevents extracting a mutable `V&` from a const source.

  // Copy from `const result&`: value types and const-lvalue-ref types.
  result(const result& that)
    requires(
        !std::is_rvalue_reference_v<T> && !is_mutable_lref_v &&
        std::is_copy_constructible_v<ref_wrapped_t>)
      : base{that} {
    if (this->eos_.is_result_has_value()) {
      new (&value_) ref_wrapped_t{that.value_};
    }
  }
  // Copy from mutable `result&`: non-const lvalue refs (deep-const).
  result(result& that)
    requires(is_mutable_lref_v)
      : base{that} {
    if (this->eos_.is_result_has_value()) {
      new (&value_) ref_wrapped_t{that.value_};
    }
  }

  // Copy-assign from `const result&`: value types and const-lvalue-ref types.
  result& operator=(const result& that)
    requires(
        !std::is_rvalue_reference_v<T> && !is_mutable_lref_v &&
        std::is_copy_constructible_v<ref_wrapped_t> &&
        std::is_copy_assignable_v<ref_wrapped_t>)
  {
    return copy_assign_from(that);
  }
  // Copy-assign from mutable `result&`: non-const lvalue refs (deep-const).
  result& operator=(result& that)
    requires(
        is_mutable_lref_v &&
        std::is_assignable_v<ref_wrapped_t&, ref_wrapped_t>)
  {
    return copy_assign_from(that);
  }

  /// Allow returning `stopped_result` from `result` coroutines & functions.
  ///
  /// This forbids `result<stopped_result_t>` (`static_assert` in CRTP).
  /*implicit*/ result(stopped_result_t s) : base{folly::error_or_stopped{s}} {}

  /// Implicitly movable / explicitly copyable from `error_or_stopped` to
  /// make it easy to return `resT1.error_or_stopped()` in a `result<T2>`
  /// function.
  ///
  /// This forbids `result<error_or_stopped>` (`static_assert` in CRTP).
  /*implicit*/ result(class error_or_stopped&& eos) : base{std::move(eos)} {}
  explicit result(const class error_or_stopped& eos) : base{eos} {}

  /// Copy- & move-conversion from a reference wrapper.
  ///
  /// Implicit to allow returning `std::ref(memberVar_)` from member functions.
  /* implicit */ result(ref_wrapped_t t) noexcept
    requires std::is_reference_v<T>
      : base{typename base::has_value_sigil_t{}} {
    new (&value_) ref_wrapped_t{std::move(t)};
  }

  /// Move-construct `result<T>` from the underlying value type `T`.
  ///
  /// Implicit to allow `result<T>` functions to return `T{}` etc.
  /* implicit */ result(T&& t) noexcept(noexcept(ref_wrapped_t{std::move(t)}))
    requires(
        !std::is_reference_v<T> && requires { ref_wrapped_t{std::move(t)}; })
      : base{typename base::has_value_sigil_t{}} {
    new (&value_) ref_wrapped_t{std::move(t)};
  }
  result& operator=(T&& t) noexcept(
      noexcept(FOLLY_DECLVAL(ref_wrapped_t&) = std::move(t)) &&
      noexcept(ref_wrapped_t{std::move(t)}))
    requires(
        !std::is_reference_v<T> &&
        requires {
          FOLLY_DECLVAL(ref_wrapped_t&) = std::move(t);
          ref_wrapped_t{std::move(t)};
        })
  {
    if (this->eos_.is_result_has_value()) {
      value_ = std::move(t);
    } else {
      // Construct first; if it throws, we stay in error state (no valueless).
      new (&value_) ref_wrapped_t{std::move(t)};
      this->eos_ = folly::error_or_stopped{typename base::has_value_sigil_t{}};
    }
    return *this;
  }

  /// Copy underlying `T`, but ONLY when small & trivially copyable.
  /// Implicit, so that e.g. `result<int> memberFn()` can return `memVar_`.
  //
  /// These are a special case because such copies are cheap*, and because
  /// good alternatives for populating trivially copyable data are few:
  ///   - Copy-construct the value into the `result`.
  ///   - Less efficient: Default-initialize the `result` and assign
  ///     `folly::copy(T)`, or use a mutable value reference to populate it.
  ///   - Future: Implement in-place construction, to handle very hot code.
  ///
  /// This constructor is deliberately restricted to objects that fit in a
  /// cache-line.  This is a heuristic to require larger copies to be explicit
  /// via `folly::copy()`.  If it proves fragile across different architectures,
  /// it can be relaxed later.
  ///
  /// Notes:
  ///   - For now, we omitted the analogous ctor to copy `result<V&>`, for the
  ///     reason that it's much less common than wanting to write e.g.
  ///     `result<int> r{intVar}`.  It can be added later if strongly needed.
  ///   - We don't need copy ctors for `co_return varOfTypeT;` because this is
  ///     an "implicitly movable context" in the C++ spec, so a move ctor is
  ///     automatically considered as the first option.
  /* implicit */ result(const T& t) noexcept(noexcept(ref_wrapped_t{t}))
    requires(
        !std::is_reference_v<T> && requires { ref_wrapped_t{t}; } &&
        std::is_trivially_copyable_v<T> &&
        sizeof(T) <= hardware_constructive_interference_size)
      : base{typename base::has_value_sigil_t{}} {
    new (&value_) ref_wrapped_t{t};
  }

  /// Simple copy/move conversion (see also the fallible conversion below).
  ///
  /// Convert `result<U>` to `result<T>` if:
  ///   - `U` is a value type that is copy/move convertible to `T`.
  ///   - `U` is a reference whose ref-wrapper is convertible to `T`.
  /// The test `simpleConversion` shows why this was made implicit.
  ///
  /// In hot code, prefer to convert from an rvalue (move conversion), because
  /// that avoids the ~7ns atomic overhead of copying the `std::exception_ptr`.
  template <class Arg, typename ResultT = std::remove_cvref_t<Arg>>
    requires(
        !std::is_same_v<ResultT, result> && // Not a move/copy ctor
        is_instantiation_of_v<::folly::result, ResultT> &&
        // Check that source value type is convertible to our storage type.
        //
        // FIXME(clang-ice): This would be faster to build & easier to read,
        // but it trips an ICE in a particular LLVM-20 configuration:
        //   requires { ref_wrapped_t(FOLLY_DECLVAL(Arg&&).value_); }
        // The compiler assertion failure occurs when a constructor's
        // `requires` clause accesses an anonymous union member, and the class
        // has deduction guides.  Until that compiler is patched & released, we
        // have to use this worse formulation.
        std::is_constructible_v<
            ref_wrapped_t,
            like_t<Arg &&, typename ResultT::storage_type>>)
  /* implicit */ result(Arg&& that) : base{std::forward<Arg>(that).eos_} {
    if (this->eos_.is_result_has_value()) {
      // Uses () instead of {} to match standard C++ implicit conversion
      // behavior, e.g. `co_return 129` in `result<uint8_t>`.
      // See `Result.conversionTypes` test.
      new (&value_) ref_wrapped_t(std::forward<Arg>(that).value_);
    }
  }

  /// Fallible copy/move conversion
  ///
  /// If a user type has a fallible conversion: `U` -> `result<T>`, implicitly
  /// convert `result<U>` into `result<T>`, and rewrap any conversion error.
  /// The test `fallibleConversion` explains why it has to be **implicit**.
  ///
  /// This helps with `for` loops that iterate over `result<U>`.  This loop:
  ///   auto uGen = generate_result<U>();
  ///   for (result<T> mv: uGen) {}
  /// expands to:
  ///   result<T> mv = *loopIter;
  /// The RHS is usually `result<U>&`, or `result<U>&&` if `U` is an rref.
  ///
  /// Fallible conversion from other result types (via rewrapping).
  template <class Arg, typename ResultT = std::remove_cvref_t<Arg>>
    requires(
        !std::is_same_v<ResultT, result> && // Not a move/copy ctor.
        is_instantiation_of_v<::folly::result, ResultT> && // Must be a result.
        // Avoid ambiguity with the above "simple conversion": if our
        // storage_type is constructible from theirs, use simple conversion.
        !std::is_constructible_v< // FIXME(clang-ice): See better code above.
            ref_wrapped_t, like_t<Arg&&, typename ResultT::storage_type>> &&
        // The value types must be compatible for rewrapping to work.
        // This includes both direct T construction AND conversion operators.
        std::is_constructible_v<
            result,
            like_t<Arg &&, typename ResultT::value_type>>)
  /*implicit*/ result(Arg&& rt)
      : result(base::rewrapping_result_convert(std::forward<Arg>(rt))) {}

  friend bool operator==(const result& a, const result& b)
    requires requires(const ref_wrapped_t& v) { v == v; }
  {
    if (a.has_value()) {
      return b.has_value() && a.value_ == b.value_;
    }
    return !b.has_value() && a.eos_ == b.eos_;
  }

  /// Retrieve non-reference `T`
  [[nodiscard]] const T& value_or_throw() const&
    requires(!std::is_reference_v<T>)
  {
    this->throw_if_no_value();
    return this->value_;
  }
  [[nodiscard]] T& value_or_throw() &
    requires(!std::is_reference_v<T>)
  {
    this->throw_if_no_value();
    return this->value_;
  }
  [[nodiscard]] const T&& value_or_throw() const&&
    requires(!std::is_reference_v<T>)
  {
    this->throw_if_no_value();
    return std::move(this->value_);
  }
  [[nodiscard]] T&& value_or_throw() &&
    requires(!std::is_reference_v<T>)
  {
    this->throw_if_no_value();
    return std::move(this->value_);
  }

  /// Retrieve reference `T`.
  ///
  /// NB Unlike the value-type versions, accessors cannot mutate the reference
  /// wrapper inside `this`.  Assign a ref-wrapper to the `result` to do that.

  /// Lvalue result-ref propagate `const`: `const result<T&>` -> `const T&`.
  /// See a discussion of the trade-offs in `docs/result.md` & `design_notes.md`
  [[nodiscard]] like_t<const int&, T> value_or_throw() const&
    requires std::is_lvalue_reference_v<T>
  {
    this->throw_if_no_value();
    return std::as_const(this->value_.get());
  }
  [[nodiscard]] T value_or_throw() &
    requires std::is_lvalue_reference_v<T>
  {
    this->throw_if_no_value();
    return this->value_.get();
  }
  [[nodiscard]] T value_or_throw() &&
    requires std::is_lvalue_reference_v<T>
  {
    this->throw_if_no_value();
    return this->value_.get();
  }

  // R-value refs follow `folly::rvalue_reference_wrapper`.  They model
  // single-use references, and thus require `&&` qualification.
  [[nodiscard]] T value_or_throw() &&
    requires std::is_rvalue_reference_v<T>
  {
    this->throw_if_no_value();
    return std::move(this->value_).get();
  }

  /// Pointer to the contained value, or `nullptr` for error/stopped.
  ///
  /// Prefer this over `has_value()` + `value_or_throw()` when branching on an
  /// lvalue `result`. Rvalue overloads are deleted to avoid pointers into
  /// temporaries or single-use rvalue references.
  [[nodiscard]] auto get_pointer() const& noexcept
    requires(!std::is_rvalue_reference_v<T>)
  {
    return this->has_value() ? std::addressof(this->value_or_throw()) : nullptr;
  }

  [[nodiscard]] auto get_pointer() & noexcept
    requires(!std::is_rvalue_reference_v<T>)
  {
    return this->has_value() ? std::addressof(this->value_or_throw()) : nullptr;
  }

  void get_pointer() && = delete;

  /// PRIVATE: See comments in the base class.
  explicit result(typename base::has_value_sigil_t s) noexcept : base{s} {}
  result(typename base::has_value_sigil_t s, result*& ptr) noexcept : base{s} {
    ptr = this;
  }
};

template <typename T>
result(std::reference_wrapper<T>) -> result<T&>;

template <typename T>
result(rvalue_reference_wrapper<T>) -> result<T&&>;

/// Specialization for `T = void` aka `result<>`. Uses no `value_` storage.
template <>
class [[nodiscard]] [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE]] result<void> final
    : public detail::result_crtp<result<void>, void> {
 private:
  using base = detail::result_crtp<result<void>, void>;
  friend class detail::result_promise_return<void>;

 public:
  ~result() = default;

  /// Unlike `result<T>` (above), default-constructing `result<void>` is fine.
  /// Here, `std::expected<void>` and `Try<void>` agree on the semantics -- and
  /// this is the most obvious way to get a value-state `result<>`.
  result() : base{typename base::has_value_sigil_t{}} {}

  /// Movable
  result(result&& that) noexcept = default;
  result& operator=(result&& that) noexcept {
    if (this != &that) [[likely]] {
      this->eos_ = std::move(that.eos_);
    }
    return *this;
  }

  /// Copyable
  result(const result& that) : base{that} {}
  result& operator=(const result& that) {
    if (this != &that) [[likely]] {
      this->eos_ = that.eos_;
    }
    return *this;
  }

  /// Allow returning `stopped_result` from `result` coroutines & functions.
  /*implicit*/ result(stopped_result_t s) : base{folly::error_or_stopped{s}} {}

  /// Implicitly movable / explicitly copyable from `error_or_stopped`.
  /*implicit*/ result(class error_or_stopped&& eos) : base{std::move(eos)} {}
  explicit result(const class error_or_stopped& eos) : base{eos} {}

  /// Fallible conversion from other result types. Details in `result<T>` ctor.
  /// Accepts `result<void>` OR types convertible to `result<void>`.
  template <class Arg, typename ResultT = std::remove_cvref_t<Arg>>
    requires(
        !std::is_same_v<ResultT, result> && // Not a move/copy ctor.
        is_instantiation_of_v<::folly::result, ResultT> &&
        (std::is_void_v<typename ResultT::value_type> ||
         std::is_constructible_v<
             result,
             like_t<Arg &&, typename ResultT::value_type>>))
  /*implicit*/ result(Arg&& rt)
      : result(base::rewrapping_result_convert(std::forward<Arg>(rt))) {}

  friend bool operator==(const result& a, const result& b) {
    if (a.has_value()) {
      return b.has_value();
    }
    return !b.has_value() && a.eos_ == b.eos_;
  }

  void value_or_throw() const { this->throw_if_no_value(); }

  // PRIVATE: See `has_value_sigil_t` in base class.
  explicit result(typename base::has_value_sigil_t s) noexcept : base{s} {}
  result(typename base::has_value_sigil_t s, result*& ptr) noexcept : base{s} {
    ptr = this;
  }
};

// Type trait to test if a type is a `result`.
template <typename T>
struct is_result : std::false_type {};
template <typename T>
struct is_result<result<T>> : std::true_type {};

/// Wraps the return value from the lambda `fn` in a `result`, putting any
/// thrown exception into its "error" state.
///
///   return result_catch_all([&](){ return riskyWork(); });
///
/// Note:
///   - `result<>` coroutines catch unhandled exceptions, but can additionally
///     throw `bad_alloc` for coro frame allocations (but see LLVM PR 152623).
///   - Like all functions, `result<>` non-coroutines let exceptions fly.
template <typename F, typename RetF = decltype(FOLLY_DECLVAL(F&&)())>
// Wrap the return type of `fn` with `result` unless it already is `result`.
conditional_t<is_instantiation_of_v<result, RetF>, RetF, result<RetF>>
result_catch_all(F&& fn) noexcept {
  try {
    if constexpr (std::is_void_v<RetF>) {
      static_cast<F&&>(fn)();
      return {};
    } else {
      return static_cast<F&&>(fn)();
    }
  } catch (...) {
    return error_or_stopped::from_current_exception();
  }
}

std::ostream& operator<<(std::ostream&, const error_or_stopped&);

} // namespace folly

template <>
struct fmt::formatter<folly::error_or_stopped> {
  constexpr format_parse_context::iterator parse(format_parse_context& ctx) {
    return ctx.begin();
  }
  format_context::iterator format(
      const folly::error_or_stopped& eos, format_context& ctx) const {
    eos.format_to(ctx.out());
    return ctx.out();
  }
};

#endif // FOLLY_HAS_RESULT
