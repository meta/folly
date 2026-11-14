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

#include <folly/coro/Task.h>
#include <folly/lang/MustUseImmediately.h>

/// `TaskWrapper.h` provides base classes for wrapping `folly::coro::Task` with
/// custom functionality.  These work by composition, which avoids the pitfalls
/// of inheritance -- your custom wrapper will not be "is-a-Task", and will not
/// implicitly "object slice" to a `Task`.
///
/// The point of this header is to uniformly forward the large API surface of
/// `Task`, `TaskWithExecutor`, and `TaskPromise`, leaving just the "new logic"
/// in each wrapper's implementation.
///
///   - `TaskWrapperCrtp` makes your type (1)  a coroutine (`promise_type`)
///     that can `co_await` other `folly::coro` objects.  (2) semi-awaitable by
///     other `folly::coro` coroutines.  It has the following features:
///        * `co_await`ability (using `co_viaIfAsync`)
///        * Interoperates with `folly::coro` awaitable wrappers like
///          `co_awaitTry` and `co_nothrow`.
///        * `co_withCancellation` to add a cancellation token
///        * `co_withExecutor` to add a cancellation token
///        * Basic reflection via `folly/coro/Traits.h`
///        * Empty base optimization for zero runtime overhead
///
///   - `TaskWithExecutorWrapperCrtp` is awaitable, but not a coroutine.  It
///     has the same features, except for `co_withExecutor`.
///
/// ### WARNING: Do not blindly forward more APIs in `TaskWrapper.h`!
///
/// Several existing wrappers are immediately-awaitable (see
/// `MustUseImmediately.h`).  For those tasks (e.g. `now_task`), API
/// forwarding is risky:
///   - Do NOT forward `semi()`, `start*()`, `unwrap()`, or other methods, or
///     CPOs that take the awaitable by-reference.  All of those make it
///     trivial to accidentally break the immediately-awaitable invariant, and
///     cause lifetime bugs.
///   - When forwarding an API, use either a static method or CPO.  Then,
///     either ONLY take the awaitable by-value, or bifurcate the API on
///     `folly::ext::must_use_immediately_v<Awaitable>`, grep for examples.
///
/// If you **have** to forward an unsafe API, here are some suggestions:
///   - Only add them in your wrapper.
///   - Add them via `UnsafeTaskWrapperCrtp` deriving from `TaskWrapperCrtp`.
///   - Add boolean flags to the configuration struct, and gate the methods via
///     `requires`.  NB: You probably cannot gate these on `Derived` **not**
///     being must-use-immediately, since CRTP bases see an incomplete type.
///
/// ### WARNING: Beware of object slicing in "unwrapping" APIs
///
/// Start by reading "A note on object slicing" in `MustUseImmediately.h`.
///
/// If your wrapper is adding new members, or customizing object lifecycle
/// (dtor / copy / move / assignment), then you must:
///   - Write a custom `unsafe_mover()` as per `lang/MustUseImmediately.h`.
///   - Overload the protected `unsafeTask()` and `unsafeTaskWithExecutor()` to
///     reduce slicing risk.
///   - Take care not to slice down to the `Crtp` bases.
///
/// ### How to implement a wrapper
///
/// First, read the WARNING sections above. Then, follow one of the "Tiny"
/// examples in `TaskWrapperTest.cpp`. The important things are:
///   - Actually read the "object slicing" warning above!
///   - In most cases, you'll need to both implement a task, and customize its
///     `TaskWithExecutorT`.  If you leave that as `coro::TaskWithExecutor`,
///     some users will accidentally avoid your wrapper's effects.
///   - Tag `YourTaskWithExecutor` with `[[nodiscard]]`.
///   - Tag `YourTask` with the `FOLLY_CORO_TASK_ATTRS` attribute.  Caveat:
///     This assumes that the coro's caller will outlive it.  That is true for
///     `Task`, and almost certainly true of all sensible wrapper types.
///   - Mark your wrappers `final` to discourage inheritance and object-slicing
///     bugs.  They can still be wrapped recursively.
///
/// Future: Once this has a benchmark, see if `FOLLY_ALWAYS_INLINE` makes
/// any difference on the wrapped functions (it shouldn't).

#if FOLLY_HAS_IMMOVABLE_COROUTINES

namespace folly::coro {

namespace detail {

template <typename Wrapper>
using task_wrapper_inner_semiawaitable_t =
    typename Wrapper::folly_private_task_wrapper_inner_t;

template <typename SemiAwaitable, typename T>
inline constexpr bool is_task_or_wrapper_v =
    (!std::is_same_v<nonesuch, SemiAwaitable> && // Does not wrap Task
     (std::is_same_v<SemiAwaitable, Task<T>> || // Wraps Task
      is_task_or_wrapper_v<
          detected_t<task_wrapper_inner_semiawaitable_t, SemiAwaitable>,
          T>));

template <typename Wrapper>
using task_wrapper_inner_promise_t = typename Wrapper::TaskWrapperInnerPromise;

template <typename Promise, typename T>
inline constexpr bool is_task_promise_or_wrapper_v =
    (!std::is_same_v<nonesuch, Promise> && // Does not wrap TaskPromise
     (std::is_same_v<Promise, TaskPromise<T>> || // Wraps TaskPromise
      is_task_promise_or_wrapper_v<
          detected_t<task_wrapper_inner_promise_t, Promise>,
          T>));

template <typename T, typename WrapperTask, typename Promise>
class TaskPromiseWrapperBase {
 private:
  // Detail of `is_promise_type_punning_safe`. Implementation notes:
  //  - This needs a body because GCC doesn't want `d` referenced in a `->` type
  //    signature.
  //  - To use this with the cheaper-to-compile `FOLLY_DECLVAL`, which is
  //    `nullptr`, this must be in an unevaluated context, since patently-null
  //    static casts are special in that they discard offsets.  So, the below
  //    equality would always be true during constant evaluation.
  template <typename Me>
  static consteval auto promise_at_offset0(Me me) {
    return std::bool_constant<
        static_cast<const void*>(&me) ==
        static_cast<const void*>(&me.promise_)>{};
  }

  // CRITICAL SAFETY CHECK: `&promise_` must be the same as `this`, and both
  // objects must have the same size.  This is required since some promise
  // operations (notably `get_return_object()` need to obtain a
  // `coroutine_handle<Promise>` to allow the wrapped coro not to know about
  // the wrapper.  At the same time, the actual promise is
  // `TaskPromiseWrapperBase`.  Punning promises & handles in this way is
  // technically UB, but it's practically safe so long as the layouts of
  // `Promise` and `TaskPromiseWrapperBase` are identical, which is what we
  // verify here.
  static consteval bool is_promise_type_punning_safe() {
    return require_sizeof<Promise> == require_sizeof<TaskPromiseWrapperBase> &&
        decltype(promise_at_offset0(
            FOLLY_DECLVAL(TaskPromiseWrapperBase)))::value;
  }

 protected:
  static_assert(
      is_task_or_wrapper_v<WrapperTask, T>,
      "SemiAwaitable must be a sequence of wrappers ending in Task<T>");
  static_assert(
      is_task_promise_or_wrapper_v<Promise, T>,
      "Promise must be a sequence of wrappers ending in TaskPromise<T>");

  Promise promise_;

  TaskPromiseWrapperBase() noexcept = default;
  ~TaskPromiseWrapperBase() = default;

 public:
  using TaskWrapperInnerPromise = Promise;

  WrapperTask get_return_object() noexcept {
    // CRITICAL: This assert justifies why it is practically safe to rely on
    // the `from_promise` UB in `TaskPromiseCrtpBase::get_return_object`.
    //
    // PS The assert isn't at class scope, since the type would be incomplete.
    static_assert(is_promise_type_punning_safe());
    return WrapperTask{promise_.get_return_object()};
  }

  FOLLY_CORO_DEFINE_FRAME_ALLOCATOR(
      ::folly::coro::detail::CoroFrameAsyncAllocator)

  auto initial_suspend() noexcept { return promise_.initial_suspend(); }
  auto final_suspend() noexcept { return promise_.final_suspend(); }

  template <typename Awaitable>
    requires(!folly::ext::must_use_immediately_v<Awaitable>)
  auto await_transform(Awaitable&& what) {
    return promise_.await_transform(std::forward<Awaitable>(what));
  }
  template <typename Awaitable>
    requires folly::ext::must_use_immediately_v<Awaitable>
  auto await_transform(Awaitable what) {
    return promise_.await_transform(
        folly::ext::must_use_immediately_unsafe_mover(std::move(what))());
  }

  template <typename Value>
  auto yield_value(Value&& v)
    requires(
        !folly::ext::must_use_immediately_v<Value> &&
        requires { promise_.yield_value(std::forward<Value>(v)); })
  {
    return promise_.yield_value(std::forward<Value>(v));
  }

  template <typename Value>
  auto yield_value(Value v)
    requires(
        folly::ext::must_use_immediately_v<Value> &&
        requires {
          promise_.yield_value(
              folly::ext::must_use_immediately_unsafe_mover(std::move(v))());
        })
  {
    return promise_.yield_value(
        folly::ext::must_use_immediately_unsafe_mover(std::move(v))());
  }

  void unhandled_exception() noexcept { promise_.unhandled_exception(); }

  // These getters are all interposed for `TaskPromiseBase::FinalAwaiter`
  decltype(auto) result() { return promise_.result(); }
  decltype(auto) getAsyncFrame() { return promise_.getAsyncFrame(); }
  auto& scopeExitRef(TaskPromisePrivate tag) {
    return promise_.scopeExitRef(tag);
  }
  auto& continuationRef(TaskPromisePrivate tag) {
    return promise_.continuationRef(tag);
  }
  auto& executorRef(TaskPromisePrivate tag) {
    return promise_.executorRef(tag);
  }

  // These next two definitions forward `getErrorHandle` behavior to the
  // innermost promise object.  See `ExtendedCoroutineHandle` for docs.

  using use_extended_handle_concept = ExtendedCoroutineHandle::PrivateTag;

  static ExtendedCoroutineHandle::PromiseBase* getPromiseBase(
      ExtendedCoroutineHandle::PrivateTag priv, TaskPromiseWrapperBase* me) {
    return Promise::getPromiseBase(priv, &me->promise_);
  }
};

template <typename T, typename WrapperTask, typename Promise>
class TaskPromiseWrapper
    : public TaskPromiseWrapperBase<T, WrapperTask, Promise> {
 protected:
  TaskPromiseWrapper() noexcept = default;
  ~TaskPromiseWrapper() = default;

 public:
  template <typename U = T> // see "`co_return` with implicit ctor" test
  auto return_value(U&& value) {
    static_assert( // See `is_promise_type_punning_safe` for rationale
        require_sizeof<TaskPromiseWrapper> ==
        require_sizeof<TaskPromiseWrapperBase<T, WrapperTask, Promise>>);
    return this->promise_.return_value(std::forward<U>(value));
  }
};

template <typename WrapperTask, typename Promise>
class TaskPromiseWrapper<void, WrapperTask, Promise>
    : public TaskPromiseWrapperBase<void, WrapperTask, Promise> {
 protected:
  TaskPromiseWrapper() noexcept = default;
  ~TaskPromiseWrapper() = default;

 public:
  void return_void() noexcept {
    static_assert( // See `is_promise_type_punning_safe` for rationale
        require_sizeof<TaskPromiseWrapper> ==
        require_sizeof<TaskPromiseWrapperBase<void, WrapperTask, Promise>>);
    this->promise_.return_void();
  }
};

// Mixin for TaskWrapper.h configs for `Task` & `TaskWithExecutor` types
struct DoesNotWrapAwaitable {
  template <typename Awaitable>
  static inline constexpr Awaitable&& wrapAwaitable(Awaitable&& awaitable) {
    return static_cast<Awaitable&&>(awaitable);
  }
};

} // namespace detail

// IMPORTANT: Read "Do not blindly forward more APIs" in the file docblock.  In
// a nutshell, adding methods, or by-ref CPOs, can compromise the safety of
// immediately-awaitable wrappers, so DON'T DO THAT.
template <typename Derived, typename Cfg>
class TaskWrapperCrtp {
 public:
  using promise_type = typename Cfg::PromiseT;

  // Pass `tw` by-value, since `&&` would break immediately-awaitable types
  friend typename Cfg::TaskWithExecutorT co_withExecutor(
      Executor::KeepAlive<> executor, Derived tw) noexcept {
    return typename Cfg::TaskWithExecutorT{
        co_withExecutor(std::move(executor), std::move(tw).unwrapTask())};
  }

  // Pass `tw` by-value, since `&&` would break immediately-awaitable types
  friend Derived co_withCancellation(
      const CancellationToken& cancelToken, Derived tw) noexcept {
    return Derived{
        co_withCancellation(cancelToken, std::move(tw).unwrapTask())};
  }

  friend Derived co_withCancellation(
      CancellationToken&& cancelToken, Derived tw) noexcept {
    auto consumedToken = std::move(cancelToken);
    return Derived{co_withCancellation(
        std::move(consumedToken), std::move(tw).unwrapTask())};
  }

  // Pass `tw` by-value, since `&&` would break immediately-awaitable types
  // Has copy-pasta below in `TaskWithExecutorWrapperCrtp`.
  friend auto co_viaIfAsync(
      Executor::KeepAlive<> executor, Derived tw) noexcept {
    return Cfg::wrapAwaitable(co_viaIfAsync(
        std::move(executor),
        folly::ext::must_use_immediately_unsafe_mover(
            std::move(tw).unwrapTask())()));
  }

  // No `cpo_t<co_withAsyncStack>` since a "Task" is not an awaitable.

  using folly_private_task_wrapper_inner_t = typename Cfg::InnerTaskT;
  using folly_private_task_wrapper_crtp_base = TaskWrapperCrtp;

  // Wrappers can override these as-needed
  using folly_must_use_immediately_t =
      ext::must_use_immediately_t<typename Cfg::InnerTaskT>;
  using folly_private_value_only_awaitable_t =
      value_only_awaitable_t<typename Cfg::InnerTaskT>;
  template <safe_alias Default>
  using folly_private_safe_alias_t =
      safe_alias_of<folly_private_task_wrapper_inner_t, Default>;

 private:
  using Inner = folly_private_task_wrapper_inner_t;
  static_assert(
      detail::is_task_or_wrapper_v<Inner, typename Cfg::ValueT>,
      "*TaskWrapper must wrap a sequence of wrappers ending in Task<T>");

  Inner inner_;

 protected:
  template <typename, typename, typename> // can construct
  friend class ::folly::coro::detail::TaskPromiseWrapperBase;

  explicit TaskWrapperCrtp(Inner t) noexcept
      // NOLINTNEXTLINE(facebook-folly-coro-temporary-by-ref)
      : inner_(folly::ext::must_use_immediately_unsafe_mover(std::move(t))()) {
    static_assert(
        folly::ext::must_use_immediately_v<Derived> ||
            !folly::ext::must_use_immediately_v<
                typename Cfg::TaskWithExecutorT>,
        "`TaskWithExecutorT` must `ext::wrap_must_use_immediately_t` because the inner "
        "task did");
  }

  // See "A note on object slicing" in `MustUseImmediately.h`
  Inner unwrapTask() && noexcept {
    static_assert(sizeof(Inner) == sizeof(Derived));
    return folly::ext::must_use_immediately_unsafe_mover(std::move(inner_))();
  }

 private:
  template <typename T>
  using my_curried_mover = folly::ext::curried_unsafe_mover_t<
      T,
      decltype(folly::ext::must_use_immediately_unsafe_mover(
          FOLLY_DECLVAL(Inner)))>;

 public:
  template <typename Me> // not a forwarding ref, see requires clause
    requires(
        // This check guards against misuse (+ fails on lvalue refs)
        // See `wrap_must_use_immediately_t::unsafe_mover` for more context
        std::is_base_of_v<TaskWrapperCrtp, Me>)
  static my_curried_mover<Me> unsafe_mover(
      folly::ext::must_use_immediately_private_t /*unused*/, Me&& me) noexcept {
    return folly::ext::curried_unsafe_mover_from_bases_and_members<
        TaskWrapperCrtp>(
        folly::tag</*no bases*/>,
        folly::vtag<&TaskWrapperCrtp::inner_>,
        static_cast<Me&&>(me));
  }
  template <typename DerivedFromMe>
    requires(
        // Matches the SFINAE logic in our `unsafe_mover`
        std::is_base_of_v<TaskWrapperCrtp, DerivedFromMe>)
  explicit TaskWrapperCrtp(
      folly::ext::curried_unsafe_mover_private_t /*unused*/,
      my_curried_mover<DerivedFromMe>&& mover)
      // `must_use_immediately_unsafe_mover` has more `noexcept` assertions
      noexcept(noexcept(Inner{std::move(mover.template get<0>())()}))
      : inner_{std::move(mover.template get<0>())()} {}
};

// IMPORTANT: Read "Do not blindly forward more APIs" in the file docblock.  In
// a nutshell, adding methods, or by-ref CPOs, can compromise the safety of
// immediately-awaitable wrappers, so DON'T DO THAT.
template <typename Derived, typename Cfg>
class TaskWithExecutorWrapperCrtp {
 private:
  using Inner = typename Cfg::InnerTaskWithExecutorT;
  Inner inner_;

 protected:
  // See "A note on object slicing" in `MustUseImmediately.h`
  Inner unwrapTaskWithExecutor() && noexcept {
    static_assert(sizeof(Inner) == sizeof(Derived));
    return folly::ext::must_use_immediately_unsafe_mover(std::move(inner_))();
  }

  // Our task can construct us, and that logic lives in the CRTP base
  friend typename Cfg::WrapperTaskT::folly_private_task_wrapper_crtp_base;

  explicit TaskWithExecutorWrapperCrtp(Inner t) noexcept(noexcept(Inner{
      FOLLY_DECLVAL(Inner)}))
      : inner_(folly::ext::must_use_immediately_unsafe_mover(std::move(t))()) {}

 public:
  // This is a **deliberately undefined** declaration. It is provided so that
  // `await_result_t` can work, e.g. `AsyncScope` checks that for all tasks.
  //
  // We do NOT want a definition here, for two reasons:
  //   - As a destructive member function, this can easily violate the
  //     "immediately awaitable" invariant -- all you have to do is
  //     `twe.operator co_await()`.
  //   - A definition would have to handle `Cfg::wrapAwaitable`, but also avoid
  //     double-wrapping the awaitable (*if* that can occur?).  No definition
  //     means I don't have to think through this :)
  //
  // If, in the future, something requires `get_awaiter()` to handle a wrapped
  // task-with-executor in an **evaluated** context, we can then provide the
  // definition, being mindful of the above concerns.
  //
  // NB: Adding a definition should not let this naively wrong code compile --
  // that goes through `await_transform()`.  `NowTaskTest.cpp` checks this.
  //   auto t = co_withExecutor(ex, someNowTask());
  //   co_await std::move(t);
  auto operator co_await() && noexcept
      -> decltype(Cfg::wrapAwaitable(std::move(inner_)).operator co_await());

  // Pass `twe` by-value, since `&&` would break immediately-awaitable types
  friend Derived co_withCancellation(
      CancellationToken cancelToken, Derived twe) noexcept {
    return Derived{co_withCancellation(
        std::move(cancelToken),
        folly::ext::must_use_immediately_unsafe_mover(
            std::move(twe.inner_))())};
  }

  // Pass `twe` by-value, since `&&` would break immediately-awaitable types
  // Has copy-pasta above in `TaskWrapperCrtp`.
  friend auto co_viaIfAsync(
      Executor::KeepAlive<> executor, Derived twe) noexcept {
    return Cfg::wrapAwaitable(co_viaIfAsync(
        std::move(executor),
        folly::ext::must_use_immediately_unsafe_mover(
            std::move(twe.inner_))()));
  }

  // `AsyncScope` requires an awaitable with an executor already attached, and
  // thus directly calls `co_withAsyncStack` instead of `co_viaIfAsync`.  But,
  // we still need to wrap the awaitable on that code path.
  //
  // NB: Passing by-&& here looks like it could compromise the safety of
  // immediately-awaitable coros (`now_task`, `now_task_with_executor`).  With
  // by-value, `BlockingWaitTest.AwaitNowTaskWithExecutor` would not build.
  //
  // Supporting pass-by-value would require fixing a LOT of plumbing.
  //   - `WithAsyncStack.h` calls `is_tag_invocable_v`, which would fail on
  //     `now_task_with_executor` if this is by-value, since the implementation
  //     of `is_tag_invocable_v` presents all args by-&&.
  //   - `CommutativeWrapperAwaitable` and `StackAwareViaIfAsyncAwaiter`,
  //     among others, also assume that `co_withAsyncStack` takes by-ref.
  //
  // Fortunately, I'm not aware of any practical reduction in
  // immediately-awaitable safety from this issue.  `co_withAsyncStack` should
  // never be called in user code.  Internal usage in `folly/coro` looks
  // overall immediately-awaitable-safe -- and the best safeguard for any
  // particular scenario is to test, see e.g. `NowTaskTest.blockingWait`.
  friend auto
  tag_invoke(cpo_t<co_withAsyncStack> /*unused*/, Derived&& twe) noexcept(
      noexcept(co_withAsyncStack(FOLLY_DECLVAL(Inner)))) {
    return Cfg::wrapAwaitable(co_withAsyncStack(std::move(twe.inner_)));
  }

  using folly_must_use_immediately_t = ext::must_use_immediately_t<Inner>;
  using folly_private_task_without_executor_t = typename Cfg::WrapperTaskT;
  template <safe_alias Default>
  using folly_private_safe_alias_t = safe_alias_of<Inner, Default>;

 private:
  template <typename T>
  using my_curried_mover = folly::ext::curried_unsafe_mover_t<
      T,
      decltype(folly::ext::must_use_immediately_unsafe_mover(
          FOLLY_DECLVAL(Inner)))>;

 public:
  template <typename Me> // not a forwarding ref, see requires clause
                         // This check guards against misuse (+ fails on lvalue
                         // refs) See
                         // `wrap_must_use_immediately_t::unsafe_mover` for more
                         // context
    requires std::is_base_of_v<TaskWithExecutorWrapperCrtp, Me>
  static my_curried_mover<Me> unsafe_mover(
      folly::ext::must_use_immediately_private_t /*unused*/, Me&& me) noexcept {
    return folly::ext::curried_unsafe_mover_from_bases_and_members<
        TaskWithExecutorWrapperCrtp>(
        folly::tag</*no bases*/>,
        folly::vtag<&TaskWithExecutorWrapperCrtp::inner_>,
        static_cast<Me&&>(me));
  }
  template <typename DerivedFromMe>
  // Matches the requires clause in our `unsafe_mover`
    requires std::is_base_of_v<TaskWithExecutorWrapperCrtp, DerivedFromMe>
  explicit TaskWithExecutorWrapperCrtp(
      folly::ext::curried_unsafe_mover_private_t /*unused*/,
      my_curried_mover<DerivedFromMe>&& mover)
      // `must_use_immediately_unsafe_mover` has more `noexcept` assertions
      noexcept(noexcept(Inner{std::move(mover.template get<0>())()}))
      : inner_{std::move(mover.template get<0>())()} {}
};

} // namespace folly::coro

#endif
