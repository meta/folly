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
#include <type_traits>

#include <folly/Executor.h>
#include <folly/Traits.h>
#include <folly/coro/Coroutine.h>
#include <folly/coro/Traits.h>
#include <folly/coro/WithAsyncStack.h>
#include <folly/coro/WithCancellation.h>
#include <folly/coro/detail/Malloc.h>
#include <folly/io/async/Request.h>
#include <folly/lang/CustomizationPoint.h>
#include <folly/lang/MustUseImmediately.h>
#include <folly/lang/SafeAlias-fwd.h>
#include <folly/tracing/AsyncStack.h>

#include <glog/logging.h>

#if FOLLY_HAS_COROUTINES

namespace folly::coro {

namespace detail {

class ViaCoroutinePromiseBase {
 public:
  suspend_always initial_suspend() noexcept { return {}; }

  void return_void() noexcept {}

  [[noreturn]] void unhandled_exception() noexcept {
    folly::assume_unreachable();
  }

  void setExecutor(folly::Executor::KeepAlive<> executor) noexcept {
    executor_ = std::move(executor);
  }

  void setContinuation(ExtendedCoroutineHandle continuation) noexcept {
    continuation_ = continuation;
  }

  void setParentFrameUnsafe(folly::AsyncStackFrame& parentFrame) noexcept {
    leafFrame_.setParentFrameUnsafe(parentFrame);
  }

  void setReturnAddress(void* returnAddress) noexcept {
    leafFrame_.setReturnAddress(returnAddress);
  }

  folly::AsyncStackFrame& getLeafFrame() noexcept { return leafFrame_; }

  void setRequestContext(
      std::shared_ptr<folly::RequestContext> context) noexcept {
    context_ = std::move(context);
  }

 protected:
  void scheduleContinuation() noexcept {
    executor_->add([this]() noexcept { this->executeContinuation(); });
  }

 private:
  void executeContinuation() noexcept {
    RequestContextScopeGuard contextScope{std::move(context_)};
    if (folly::isSuspendedLeafActive(leafFrame_)) {
      folly::deactivateSuspendedLeaf(leafFrame_);
    }
    if (leafFrame_.getParentFrameUnsafe()) {
      folly::resumeCoroutineWithNewAsyncStackRoot(
          continuation_.getHandle(), *leafFrame_.getParentFrameUnsafe());
    } else {
      continuation_.resume();
    }
  }

 protected:
  ~ViaCoroutinePromiseBase() = default;

  folly::Executor::KeepAlive<> executor_;
  ExtendedCoroutineHandle continuation_;
  folly::AsyncStackFrame leafFrame_;
  std::shared_ptr<RequestContext> context_;
};

template <bool IsStackAware>
class ViaCoroutine {
 public:
  class promise_type final
      : public ViaCoroutinePromiseBase,
        public ExtendedCoroutinePromiseCrtp<promise_type> {
    struct FinalAwaiter {
      bool await_ready() noexcept { return false; }

      // This code runs immediately after the inner awaitable resumes its fake
      // continuation, and it schedules the real continuation on the awaiter's
      // executor
      FOLLY_CORO_AWAIT_SUSPEND_NONTRIVIAL_ATTRIBUTES void await_suspend(
          coroutine_handle<promise_type> h) noexcept {
        auto& promise = h.promise();
        if (!promise.context_) {
          promise.setRequestContext(RequestContext::saveContext());
        }

        if constexpr (IsStackAware) {
          folly::deactivateAsyncStackFrame(promise.getAsyncFrame());
        }

        promise.scheduleContinuation();
      }

      [[noreturn]] void await_resume() noexcept { folly::assume_unreachable(); }
    };

   public:
    FOLLY_CORO_DEFINE_FRAME_ALLOCATOR(
        ::folly::coro::detail::CoroFrameAsyncAllocator)

    ViaCoroutine get_return_object() noexcept {
      return ViaCoroutine{coroutine_handle<promise_type>::from_promise(*this)};
    }

    FinalAwaiter final_suspend() noexcept { return {}; }

    template <bool IsStackAware2 = IsStackAware>
      requires IsStackAware2
    folly::AsyncStackFrame& getAsyncFrame() noexcept {
      DCHECK(this->leafFrame_.getParentFrameUnsafe() != nullptr);
      return *this->leafFrame_.getParentFrameUnsafe();
    }

    folly::AsyncStackFrame& getLeafFrame() noexcept { return leafFrame_; }

    static std::optional<ExtendedCoroutineHandle::ErrorHandle>
    getErrorHandleImpl(promise_type& me, exception_wrapper& ex) {
      auto [handle, frame] = me.continuation_.getErrorHandle(ex);
      me.setContinuation(handle);
      if (frame && IsStackAware) {
        me.leafFrame_.setParentFrameUnsafe(*frame);
      }
      return std::nullopt;
    }
  };

  ViaCoroutine(ViaCoroutine&& other) noexcept
      : coro_(std::exchange(other.coro_, {})) {}

  ~ViaCoroutine() {
    if (coro_) {
      coro_.destroy();
    }
  }

  static ViaCoroutine create(folly::Executor::KeepAlive<> executor) {
    ViaCoroutine coroutine = createImpl();
    coroutine.setExecutor(std::move(executor));
    return coroutine;
  }

  void setExecutor(folly::Executor::KeepAlive<> executor) noexcept {
    coro_.promise().setExecutor(std::move(executor));
  }

  void setContinuation(ExtendedCoroutineHandle continuation) noexcept {
    coro_.promise().setContinuation(continuation);
  }

  void setParentFrameUnsafe(folly::AsyncStackFrame& frame) noexcept {
    coro_.promise().setParentFrameUnsafe(frame);
  }

  void setReturnAddress(void* returnAddress) noexcept {
    coro_.promise().setReturnAddress(returnAddress);
  }

  folly::AsyncStackFrame& getLeafFrame() noexcept {
    return coro_.promise().getLeafFrame();
  }

  void destroy() noexcept {
    if (coro_) {
      std::exchange(coro_, {}).destroy();
    }
  }

  void saveContext() noexcept {
    coro_.promise().setRequestContext(folly::RequestContext::saveContext());
  }

  coroutine_handle<promise_type> getHandle() noexcept { return coro_; }

 private:
  explicit ViaCoroutine(coroutine_handle<promise_type> coro) noexcept
      : coro_(coro) {}

  static ViaCoroutine createImpl() { co_return; }

  coroutine_handle<promise_type> coro_;
};

} // namespace detail

template <typename Awaitable>
class StackAwareViaIfAsyncAwaiter {
  using WithAsyncStackAwaitable =
      decltype(folly::coro::co_withAsyncStack(std::declval<Awaitable>()));
  using Awaiter = folly::coro::awaiter_type_t<WithAsyncStackAwaitable>;
  using CoroutineType = detail::ViaCoroutine<true>;
  using CoroutinePromise = typename CoroutineType::promise_type;
  using WrapperHandle = coroutine_handle<CoroutinePromise>;

  using await_suspend_result_t =
      decltype(std::declval<Awaiter&>().await_suspend(
          std::declval<WrapperHandle>()));

 public:
  explicit StackAwareViaIfAsyncAwaiter(
      folly::Executor::KeepAlive<> executor, Awaitable&& awaitable)
      : viaCoroutine_(CoroutineType::create(std::move(executor))),
        awaitable_(
            folly::coro::co_withAsyncStack(
                static_cast<Awaitable&&>(awaitable))),
        awaiter_(
            get_awaiter(static_cast<WithAsyncStackAwaitable&&>(awaitable_))) {}

  decltype(auto) await_ready() noexcept(noexcept(awaiter_.await_ready())) {
    return awaiter_.await_ready();
  }

  template <typename Promise>
  auto await_suspend(coroutine_handle<Promise> h) noexcept(noexcept(
      std::declval<Awaiter&>().await_suspend(std::declval<WrapperHandle>())))
      -> await_suspend_result_t {
    auto& promise = h.promise();
    auto& asyncFrame = promise.getAsyncFrame();

    viaCoroutine_.setContinuation(h);
    viaCoroutine_.setParentFrameUnsafe(asyncFrame);

    if constexpr (!detail::is_coroutine_handle_v<await_suspend_result_t>) {
      viaCoroutine_.saveContext();
    }

    return awaiter_.await_suspend(viaCoroutine_.getHandle());
  }

  decltype(auto) await_resume() noexcept(noexcept(awaiter_.await_resume())) {
    viaCoroutine_.destroy();
    return awaiter_.await_resume();
  }

  template <
      typename Awaiter2 = Awaiter,
      typename Result = decltype(std::declval<Awaiter2&>().await_resume_try())>
  Result await_resume_try() noexcept(
      noexcept(std::declval<Awaiter&>().await_resume_try())) {
    viaCoroutine_.destroy();
    return awaiter_.await_resume_try();
  }

#if FOLLY_HAS_RESULT
  template <
      typename Awaiter2 = Awaiter,
      typename Result =
          decltype(FOLLY_DECLVAL(Awaiter2&).await_resume_result())>
  Result await_resume_result() noexcept(
      noexcept(FOLLY_DECLVAL(Awaiter2&).await_resume_result())) {
    viaCoroutine_.destroy();
    return awaiter_.await_resume_result();
  }
#endif

 private:
  CoroutineType viaCoroutine_;
  WithAsyncStackAwaitable awaitable_;
  Awaiter awaiter_;
};

template <bool IsCallerAsyncStackAware, typename Awaitable>
class ViaIfAsyncAwaiter {
  using Awaiter = folly::coro::awaiter_type_t<Awaitable>;
  using CoroutineType = detail::ViaCoroutine<false>;
  using CoroutinePromise = typename CoroutineType::promise_type;
  using WrapperHandle = coroutine_handle<CoroutinePromise>;

  using await_suspend_result_t =
      decltype(std::declval<Awaiter&>().await_suspend(
          std::declval<WrapperHandle>()));

 public:
  explicit ViaIfAsyncAwaiter(
      folly::Executor::KeepAlive<> executor, Awaitable&& awaitable)
      : viaCoroutine_(CoroutineType::create(std::move(executor))),
        awaiter_(get_awaiter(static_cast<Awaitable&&>(awaitable))) {}

  decltype(auto) await_ready() noexcept(noexcept(awaiter_.await_ready())) {
    return awaiter_.await_ready();
  }

  // NOTE: We are using a heuristic here to determine when is the correct
  // time to capture the RequestContext. We want to capture the context just
  // before the coroutine suspends and execution is returned to the executor.
  //
  // In cases where we are awaiting another coroutine and symmetrically
  // transferring execution to another coroutine we are not yet returning
  // execution to the executor so we want to defer capturing the context until
  // the ViaCoroutine is resumed and suspends in final_suspend() before
  // scheduling the resumption on the executor.
  //
  // In cases where the awaitable may suspend without transferring execution
  // to another coroutine and will therefore return back to the executor we
  // want to capture the execution context before calling into the wrapped
  // awaitable's await_suspend() method (since it's await_suspend() method
  // might schedule resumption on another thread and could resume and destroy
  // the ViaCoroutine before the await_suspend() method returns).
  //
  // The heuristic is that if await_suspend() returns a coroutine_handle
  // then we assume it's the first case. Otherwise if await_suspend() returns
  // void/bool then we assume it's the second case.
  //
  // This heuristic isn't perfect since a coroutine_handle-returning
  // await_suspend() method could return noop_coroutine() in which case we
  // could fail to capture the current context. Awaitable types that do this
  // would need to provide a custom implementation of co_viaIfAsync() that
  // correctly captures the RequestContext to get correct behaviour in this
  // case.

  // NO_INLINE is required here because we capture the return address of the
  // calling coroutine
  template <typename Promise>
  FOLLY_NOINLINE auto
  await_suspend(coroutine_handle<Promise> continuation) noexcept(noexcept(
      std::declval<Awaiter&>().await_suspend(std::declval<WrapperHandle>())))
      -> await_suspend_result_t {
    viaCoroutine_.setContinuation(continuation);

    if constexpr (!detail::is_coroutine_handle_v<await_suspend_result_t>) {
      viaCoroutine_.saveContext();
    }

    if constexpr (IsCallerAsyncStackAware) {
      auto& asyncFrame = continuation.promise().getAsyncFrame();
      auto& stackRoot = *asyncFrame.getStackRoot();

      viaCoroutine_.setParentFrameUnsafe(asyncFrame);
      viaCoroutine_.setReturnAddress(FOLLY_ASYNC_STACK_RETURN_ADDRESS());

      folly::deactivateAsyncStackFrame(asyncFrame);
      folly::activateSuspendedLeaf(viaCoroutine_.getLeafFrame());

      // Reactivate the stack-frame before we resume.
      auto rollback = makeGuard([&] {
        folly::activateAsyncStackFrame(stackRoot, asyncFrame);
        folly::deactivateSuspendedLeaf(viaCoroutine_.getLeafFrame());
      });
      if constexpr (std::is_same_v<await_suspend_result_t, bool>) {
        if (!awaiter_.await_suspend(viaCoroutine_.getHandle())) {
          return false;
        }
        rollback.dismiss();
        return true;
      } else if constexpr (std::is_same_v<await_suspend_result_t, void>) {
        awaiter_.await_suspend(viaCoroutine_.getHandle());
        rollback.dismiss();
        return;
      } else {
        auto ret = awaiter_.await_suspend(viaCoroutine_.getHandle());
        rollback.dismiss();
        return ret;
      }
    } else {
      return awaiter_.await_suspend(viaCoroutine_.getHandle());
    }
  }

  auto await_resume() noexcept(
      noexcept(std::declval<Awaiter&>().await_resume()))
      -> decltype(std::declval<Awaiter&>().await_resume()) {
    viaCoroutine_.destroy();
    return awaiter_.await_resume();
  }

  template <
      typename Awaiter2 = Awaiter,
      typename Result = decltype(std::declval<Awaiter2&>().await_resume_try())>
  Result await_resume_try() noexcept(
      noexcept(std::declval<Awaiter&>().await_resume_try())) {
    viaCoroutine_.destroy();
    return awaiter_.await_resume_try();
  }

#if FOLLY_HAS_RESULT
  template <
      typename Awaiter2 = Awaiter,
      typename Result =
          decltype(FOLLY_DECLVAL(Awaiter2&).await_resume_result())>
  Result await_resume_result() noexcept(
      noexcept(FOLLY_DECLVAL(Awaiter2&).await_resume_result())) {
    viaCoroutine_.destroy();
    return awaiter_.await_resume_result();
  }
#endif

 private:
  CoroutineType viaCoroutine_;
  Awaiter awaiter_;
};

template <typename Awaitable>
class StackAwareViaIfAsyncAwaitable {
 public:
  explicit StackAwareViaIfAsyncAwaitable(
      folly::Executor::KeepAlive<> executor,
      Awaitable&&
          awaitable) noexcept(std::is_nothrow_move_constructible<Awaitable>::
                                  value)
      : executor_(std::move(executor)),
        awaitable_(static_cast<Awaitable&&>(awaitable)) {}

  auto operator co_await() && {
    if constexpr (is_awaitable_async_stack_aware_v<Awaitable>) {
      return StackAwareViaIfAsyncAwaiter<Awaitable>{
          std::move(executor_), static_cast<Awaitable&&>(awaitable_)};
    } else {
      return ViaIfAsyncAwaiter<true, Awaitable>{
          std::move(executor_), static_cast<Awaitable&&>(awaitable_)};
    }
  }

 private:
  folly::Executor::KeepAlive<> executor_;
  Awaitable awaitable_;
};

template <typename Awaitable>
class ViaIfAsyncAwaitable {
 public:
  explicit ViaIfAsyncAwaitable(
      folly::Executor::KeepAlive<> executor,
      Awaitable&&
          awaitable) noexcept(std::is_nothrow_move_constructible<Awaitable>::
                                  value)
      : executor_(std::move(executor)),
        awaitable_(static_cast<Awaitable&&>(awaitable)) {}

  ViaIfAsyncAwaiter<false, Awaitable> operator co_await() && {
    return ViaIfAsyncAwaiter<false, Awaitable>{
        std::move(executor_), static_cast<Awaitable&&>(awaitable_)};
  }

  friend StackAwareViaIfAsyncAwaitable<Awaitable> tag_invoke(
      cpo_t<co_withAsyncStack>, ViaIfAsyncAwaitable&& self) {
    return StackAwareViaIfAsyncAwaitable<Awaitable>{
        std::move(self.executor_), static_cast<Awaitable&&>(self.awaitable_)};
  }

 private:
  folly::Executor::KeepAlive<> executor_;
  Awaitable awaitable_;
};

namespace detail {

template <typename SemiAwaitable, typename = void>
struct HasViaIfAsyncMethod
    : std::bool_constant<!require_sizeof<SemiAwaitable>> {};

template <typename SemiAwaitable>
  requires std::is_void_v<SemiAwaitable>
struct HasViaIfAsyncMethod<SemiAwaitable, void> : std::false_type {};

template <typename SemiAwaitable>
struct HasViaIfAsyncMethod<
    SemiAwaitable,
    std::void_t<decltype(std::declval<SemiAwaitable>().viaIfAsync(
        std::declval<folly::Executor::KeepAlive<>>()))>> : std::true_type {};

namespace adl {

template <typename SemiAwaitable>
auto co_viaIfAsync(
    folly::Executor::KeepAlive<> executor,
    SemiAwaitable&&
        awaitable) noexcept(noexcept(static_cast<SemiAwaitable&&>(awaitable)
                                         .viaIfAsync(std::move(executor))))
    -> decltype(static_cast<SemiAwaitable&&>(awaitable).viaIfAsync(
        std::move(executor))) {
  return static_cast<SemiAwaitable&&>(awaitable).viaIfAsync(
      std::move(executor));
}

template <typename Awaitable>
  requires(
      is_awaitable_v<Awaitable> && !HasViaIfAsyncMethod<Awaitable>::value &&
      !folly::ext::must_use_immediately_v<Awaitable>)
auto co_viaIfAsync(folly::Executor::KeepAlive<> executor, Awaitable&& awaitable)
    -> ViaIfAsyncAwaitable<Awaitable> {
  return ViaIfAsyncAwaitable<Awaitable>{
      std::move(executor), static_cast<Awaitable&&>(awaitable)};
}
template <typename Awaitable>
  requires(
      is_awaitable_v<Awaitable> && !HasViaIfAsyncMethod<Awaitable>::value &&
      folly::ext::must_use_immediately_v<Awaitable>)
auto co_viaIfAsync(folly::Executor::KeepAlive<> executor, Awaitable awaitable)
    -> ViaIfAsyncAwaitable<Awaitable> {
  return ViaIfAsyncAwaitable<Awaitable>{
      std::move(executor), std::move(awaitable)};
}

struct ViaIfAsyncFunction {
  template <typename Awaitable>
    requires(!folly::ext::must_use_immediately_v<Awaitable>)
  auto operator()(folly::Executor::KeepAlive<> executor, Awaitable&& awaitable)
      const noexcept(noexcept(co_viaIfAsync(
          std::move(executor), static_cast<Awaitable&&>(awaitable))))
          -> decltype(co_viaIfAsync(
              std::move(executor), static_cast<Awaitable&&>(awaitable))) {
    return co_viaIfAsync(
        std::move(executor), static_cast<Awaitable&&>(awaitable));
  }
  template <typename Awaitable>
    requires folly::ext::must_use_immediately_v<Awaitable>
  auto operator()(folly::Executor::KeepAlive<> executor, Awaitable awaitable)
      const noexcept(noexcept(co_viaIfAsync(
          std::move(executor),
          folly::ext::must_use_immediately_unsafe_mover(
              std::move(awaitable))())))
          -> decltype(co_viaIfAsync(
              std::move(executor),
              folly::ext::must_use_immediately_unsafe_mover(
                  std::move(awaitable))())) {
    return co_viaIfAsync(
        std::move(executor),
        folly::ext::must_use_immediately_unsafe_mover(std::move(awaitable))());
  }
};

} // namespace adl
} // namespace detail

/// Returns a new awaitable that will resume execution of the awaiting coroutine
/// on a specified executor in the case that the operation does not complete
/// synchronously.
///
/// If the operation completes synchronously then the awaiting coroutine
/// will continue execution on the current thread without transitioning
/// execution to the specified executor.
FOLLY_DEFINE_CPO(detail::adl::ViaIfAsyncFunction, co_viaIfAsync)

template <typename T>
using semi_await_awaitable_t = decltype(folly::coro::co_viaIfAsync(
    FOLLY_DECLVAL(folly::Executor::KeepAlive<>), FOLLY_DECLVAL(T)));

template <typename T, typename = void>
struct is_semi_awaitable : std::bool_constant<!require_sizeof<T>> {};

template <typename T>
  requires std::is_void_v<T>
struct is_semi_awaitable<T, void> : std::false_type {};

template <typename T>
struct is_semi_awaitable<T, std::void_t<semi_await_awaitable_t<T>>>
    : std::true_type {};

template <typename T>
constexpr bool is_semi_awaitable_v = is_semi_awaitable<T>::value;

template <typename T>
using semi_await_result_t = await_result_t<semi_await_awaitable_t<T>>;

namespace detail {

/// Mixin for awaiters that forward to an inner awaiter with value-only
/// semantics. Provides `await_ready()` and `await_suspend()` with static
/// asserts that both are noexcept. Derived classes implement `await_resume()`.
///
/// GUIDANCE FOR NEW AWAITABLES: Design `await_ready` and `await_suspend` to
/// be noexcept.  If they must check for invariant violations:
///   - If feasible, push invariant failures into `await_resume()`.
///   - Otherwise, debug-fatal on violations, and throw an error with a clearly
///     "do not catch me" naming in opt.
template <typename Awaitable>
class ValueOnlyAwaiterBase {
 public:
  using Awaiter = awaiter_type_t<Awaitable>;

 protected:
  Awaiter awaiter_;

  explicit ValueOnlyAwaiterBase(Awaitable&& awaitable)
      : awaiter_(get_awaiter(static_cast<Awaitable&&>(awaitable))) {}

 public:
  auto await_ready() noexcept -> decltype(awaiter_.await_ready()) {
    static_assert(
        noexcept(FOLLY_DECLVAL(Awaiter&).await_ready()),
        "value-only await requires noexcept await_ready(); see class doc");
    return awaiter_.await_ready();
  }

  template <typename Promise>
  auto await_suspend(coroutine_handle<Promise> coro) noexcept
      -> decltype(awaiter_.await_suspend(coro)) {
    static_assert(
        noexcept(awaiter_.await_suspend(coro)),
        "value-only await requires noexcept await_suspend(); see class doc");
    return awaiter_.await_suspend(coro);
  }
};

template <typename T>
using value_only_awaitable_of_ =
    typename T::folly_private_value_only_awaitable_t;

template <typename Void, typename T>
struct value_only_awaitable_ {
  static_assert(
      require_sizeof<T>, "`value_only_awaitable_t` on incomplete type");
  using type = std::false_type;
};

template <>
struct value_only_awaitable_<void, void> {
  using type = std::false_type;
};

template <typename T>
struct value_only_awaitable_<std::void_t<value_only_awaitable_of_<T>>, T> {
  using type = value_only_awaitable_of_<T>;
};

} // namespace detail

/// True for awaitables that only complete with value (not stopped or error)
template <typename T>
using value_only_awaitable_t =
    typename detail::value_only_awaitable_<void, T>::type;
template <typename T>
inline constexpr bool value_only_awaitable_v = value_only_awaitable_t<T>::value;

namespace detail {

template <typename Awaiter>
using detect_await_resume_try =
    decltype(FOLLY_DECLVAL(Awaiter).await_resume_try());

template <typename Awaiter>
constexpr bool is_awaiter_try = is_detected_v<detect_await_resume_try, Awaiter>;

template <typename Awaitable>
constexpr bool is_awaitable_try = is_awaiter_try<awaiter_type_t<Awaitable>>;

template <typename Awaitable>
class TryAwaiter : public ValueOnlyAwaiterBase<Awaitable> {
 public:
  static_assert(is_awaitable_try<Awaitable&&>);

  explicit TryAwaiter(Awaitable&& awaitable)
      : ValueOnlyAwaiterBase<Awaitable>(static_cast<Awaitable&&>(awaitable)) {}

  auto await_resume()
      FOLLY_DETAIL_FORWARD_BODY(this->awaiter_.await_resume_try())
};

/**
 * Common machinery for building wrappers like co_awaitTry
 * Allows the wrapper to commute with the universal wrappers like
 * co_withCancellation while keeping the corresponding awaitable on the outside
 */
template <template <typename T> typename Derived, typename T>
class CommutativeWrapperAwaitable {
 public:
  template <typename T2>
    requires(!folly::ext::must_use_immediately_v<T2>)
  explicit CommutativeWrapperAwaitable(T2&& awaitable) noexcept(
      std::is_nothrow_constructible_v<T, T2>)
      : inner_(static_cast<T2&&>(awaitable)) {}
  // MSVC moves the by-value argument of an inherited constructor, which an
  // immovable `T2` can't do, so the wrapper factories construct their derived
  // awaitables with the `std::in_place` constructor below instead.
  template <typename T2>
    requires folly::ext::must_use_immediately_v<T2>
  explicit CommutativeWrapperAwaitable(T2 awaitable) noexcept(noexcept(T{
      FOLLY_DECLVAL(T2)}))
      : inner_(
            folly::ext::must_use_immediately_unsafe_mover(
                std::move(awaitable))()) {}

  template <typename Factory>
  explicit CommutativeWrapperAwaitable(std::in_place_t, Factory&& factory)
      : inner_(factory()) {}

  // Two overloads for the CancellationToken to avoid unnecessary copies
  // (atomic refcount costs).
  //
  // NB: If we merged the overloads into a single template, overload resolution
  // rules would consider it ambiguous wrt the default implementation in
  // `WithCancellation.h`.
  template <
      typename T2 = T,
      // "WART:" in `WithCancellation.h` explains the remove-reference
      typename Result =
          std::remove_reference_t<decltype(folly::coro::co_withCancellation(
              FOLLY_DECLVAL(const folly::CancellationToken&),
              FOLLY_DECLVAL(T2)))>>
  friend Derived<Result> co_withCancellation(
      const folly::CancellationToken& cancelToken, Derived<T> awaitable) {
    return Derived<Result>{
        std::in_place, [&]() {
          return folly::coro::co_withCancellation(
              cancelToken,
              folly::ext::must_use_immediately_unsafe_mover(
                  std::move(awaitable.inner_))());
        }};
  }
  template <
      typename T2 = T,
      // "WART:" in `WithCancellation.h` explains the remove-reference
      typename Result =
          std::remove_reference_t<decltype(folly::coro::co_withCancellation(
              FOLLY_DECLVAL(folly::CancellationToken&&), FOLLY_DECLVAL(T2)))>>
  friend Derived<Result> co_withCancellation(
      folly::CancellationToken&& cancelToken, Derived<T> awaitable) {
    return Derived<Result>{
        std::in_place, [&]() {
          return folly::coro::co_withCancellation(
              std::move(cancelToken),
              folly::ext::must_use_immediately_unsafe_mover(
                  std::move(awaitable.inner_))());
        }};
  }

  template <
      typename T2 = T,
      typename Result =
          decltype(folly::coro::co_withAsyncStack(std::declval<T2>()))>
  friend Derived<Result>
  tag_invoke(cpo_t<co_withAsyncStack>, Derived<T>&& awaitable) noexcept(
      noexcept(folly::coro::co_withAsyncStack(std::declval<T2>()))) {
    return Derived<Result>{
        std::in_place, [&]() -> decltype(auto) {
          return folly::coro::co_withAsyncStack(
              static_cast<T&&>(awaitable.inner_));
        }};
  }

  template <typename T2 = T, typename Result = semi_await_awaitable_t<T2>>
  friend Derived<Result> co_viaIfAsync(
      folly::Executor::KeepAlive<> executor,
      Derived<T> awaitable) //
      noexcept(noexcept(folly::coro::co_viaIfAsync(
          FOLLY_DECLVAL(folly::Executor::KeepAlive<>), FOLLY_DECLVAL(T2)))) {
    return Derived<Result>{
        std::in_place, [&]() {
          return folly::coro::co_viaIfAsync(
              std::move(executor),
              folly::ext::must_use_immediately_unsafe_mover(
                  std::move(awaitable.inner_))());
        }};
  }

  // IMPORTANT: If a commutative wrapper changes safety, immediate- or
  // noexcept-awaitability, it must remember to override these:
  using folly_must_use_immediately_t = ext::must_use_immediately_t<T>;
  using folly_private_value_only_awaitable_t = value_only_awaitable_t<T>;
  template <safe_alias Default>
  using folly_private_safe_alias_t = safe_alias_of<T, Default>;

 protected:
  T inner_;

 private:
  template <typename U>
  using my_curried_mover = folly::ext::curried_unsafe_mover_t<
      U,
      decltype(folly::ext::must_use_immediately_unsafe_mover(
          FOLLY_DECLVAL(T)))>;

 public:
  template <
      typename Me, // not a forwarding ref, see SFINAE
      typename T2 = T>
    requires(
        // This check guards against misuse (+ fails on lvalue refs)
        // See `wrap_must_use_immediately_t::unsafe_mover` for more context
        std::is_base_of_v<CommutativeWrapperAwaitable, Me> &&
        // Without this check we might instantiate this for things like
        // `TryAwaitable<coro::Future<...>&&>`, erroring with:
        //   "cannot form a pointer-to-member to member of reference type"
        folly::ext::must_use_immediately_v<T2>)
  static my_curried_mover<Me> unsafe_mover(
      folly::ext::must_use_immediately_private_t, Me&& me) noexcept {
    return folly::ext::curried_unsafe_mover_from_bases_and_members<
        CommutativeWrapperAwaitable>(
        folly::tag</*no bases*/>,
        folly::vtag<&CommutativeWrapperAwaitable::inner_>,
        static_cast<Me&&>(me));
  }
  template <typename DerivedFromMe>
    requires(
        // Matches the SFINAE logic in our `unsafe_mover`
        std::is_base_of_v<CommutativeWrapperAwaitable, DerivedFromMe>)
  explicit CommutativeWrapperAwaitable(
      folly::ext::curried_unsafe_mover_private_t,
      my_curried_mover<DerivedFromMe>&& mover)
      // `must_use_immediately_unsafe_mover` has more `noexcept` assertions
      noexcept(noexcept(T{std::move(mover.template get<0>())()}))
      : inner_{std::move(mover.template get<0>())()} {}
};

/// TryAwaitable
///
/// The awaitable returned by `co_awaitTry`.
///
/// `co_await co_awaitTry(awaitable)` completes with a `Try<T>` holding either
/// the value or the error of `awaitable`. Unlike a plain `co_await`, it never
/// rethrows the error into the awaiting coroutine.
///
///   auto res = co_await co_awaitTry(fetch());
///   if (res.hasException()) {
///     // inspect, log, retry, or propagate res.exception()
///   }
///
/// ## Why this exists: the error path must stay cheap
///
/// A plain `co_await` of a failed task rethrows the error. Each throw costs on
/// the order of microseconds, and with some toolchains the unwinder serializes
/// across threads. The cost repeats at every frame of a coroutine stack, even
/// with no `try` / `catch` spelled anywhere: an exception escaping a coroutine
/// body is caught by its promise's `unhandled_exception()`, stored, and then
/// rethrown by the plain `co_await` in the parent. An error passing up through
/// N plain `co_await`s costs N throws.
///
/// Error paths tend to become hot precisely when a service is overloaded:
/// timeouts, cancellations, and failing dependencies arrive in bursts. If each
/// error costs a throw, overload makes every error more expensive, which
/// deepens the overload -- a feedback loop that can turn a brownout into an
/// outage. `co_awaitTry` keeps the error in an `exception_wrapper`, so the
/// error path costs about the same as the happy path.
///
/// ## Guidance for generic libraries
///
/// Code that awaits arbitrary caller-supplied awaitables (combinators, retry,
/// timeout, and other wrappers) cannot know how hot its callers' error paths
/// are, so it must not throw on their behalf:
///
///  - To inspect errors, use `co_awaitTry` (or, in new code,
///    `value_or_error_or_stopped` from `ValueOrError.h`), not
///    `try { co_await ... } catch (...)`.
///  - To propagate a captured error, use `co_yield co_error(ew)` from
///    `Error.h` or `co_yield co_result(std::move(tryOrResult))` from
///    `Result.h`, never `throw` or `Try::value()`.
///  - To propagate without inspecting, use `co_await co_nothrow(awaitable)`
///    from `Nothrow.h`.
///
/// Following these at every layer keeps an error from being thrown at all
/// between the frame that raised it and the frame that handles it.
///
/// ## Do not "optimize" this away
///
/// Replacing `co_awaitTry` plus `hasException()` with `try` / `catch` around a
/// plain `co_await` is not a performance improvement. It saves at most a few
/// nanoseconds on the happy path (constructing and moving a `Try`) while
/// adding a throw and unwind to every error -- often on exactly the path, such
/// as a timeout, that runs hottest under load. Uses of `co_awaitTry` should be
/// presumed deliberate. Do not remove one unless profiling shows both that the
/// `Try` dominates the happy path and that the error path is cold even under
/// overload.
template <typename T>
class [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE]]
TryAwaitable : public CommutativeWrapperAwaitable<TryAwaitable, T> {
 public:
  using CommutativeWrapperAwaitable<TryAwaitable, T>::
      CommutativeWrapperAwaitable;

  template <typename Self, typename T2 = like_t<Self, T>>
    requires(
        std::is_same_v<std::remove_cvref_t<Self>, TryAwaitable> &&
        is_awaitable_v<T2>)
  friend TryAwaiter<T2> operator co_await(Self&& self) {
    return TryAwaiter<T2>{static_cast<Self&&>(self).inner_};
  }

  using folly_private_value_only_awaitable_t = std::true_type;
};

} // namespace detail

/// co_awaitTry
///
/// Await `awaitable`, completing with a `Try<T>` instead of rethrowing any
/// error. See `detail::TryAwaitable` above for when and why to use this, and
/// why uses of it should not be replaced with `try` / `catch`.
template <typename Awaitable>
  requires(!folly::ext::must_use_immediately_v<Awaitable>)
detail::TryAwaitable<std::remove_cvref_t<Awaitable>> co_awaitTry(
    [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE_ARGUMENT]] Awaitable&& awaitable) {
  return detail::TryAwaitable<std::remove_cvref_t<Awaitable>>{
      static_cast<Awaitable&&>(awaitable)};
}
/// co_awaitTry
///
/// Overload for immediately-awaitable arguments. See `detail::TryAwaitable`.
template <typename Awaitable>
  requires folly::ext::must_use_immediately_v<Awaitable>
detail::TryAwaitable<Awaitable> co_awaitTry(
    [[FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE_ARGUMENT]] Awaitable awaitable) {
  return detail::TryAwaitable<Awaitable>{
      std::in_place, [&] {
        return folly::ext::must_use_immediately_unsafe_mover(
            std::move(awaitable))();
      }};
}

template <typename T>
using semi_await_try_result_t = await_result_t<semi_await_awaitable_t<
    decltype(folly::coro::co_awaitTry(FOLLY_DECLVAL(T)))>>;

} // namespace folly::coro

#endif // FOLLY_HAS_COROUTINES
