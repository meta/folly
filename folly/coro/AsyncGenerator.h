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
// Docs: https://fburl.com/fbcref_asyncgenerator
//

#pragma once

#include <folly/CancellationToken.h>
#include <folly/ExceptionWrapper.h>
#include <folly/Traits.h>
#include <folly/Try.h>
#include <folly/coro/AutoCleanup-fwd.h>
#include <folly/coro/BasePromise.h>
#include <folly/coro/Coroutine.h>
#include <folly/coro/Invoke.h>
#include <folly/coro/Result.h>
#include <folly/coro/ScopeExit.h>
#include <folly/coro/ViaIfAsync.h>
#include <folly/coro/WithAsyncStack.h>
#include <folly/coro/WithCancellation.h>
#include <folly/coro/detail/Malloc.h>
#include <folly/coro/detail/ManualLifetime.h>
#include <folly/lang/SafeAlias-fwd.h>
#include <folly/tracing/AsyncStack.h>

#include <glog/logging.h>

#include <functional>
#include <iterator>
#include <type_traits>

#if FOLLY_HAS_COROUTINES

namespace folly {
namespace coro {
namespace detail {

template <typename Reference, typename Value, bool RequiresCleanup>
class AsyncGeneratorPromise;

} // namespace detail

/**
 * The AsyncGenerator class represents a sequence of asynchronously produced
 * values where the values are produced by a coroutine.
 *
 * Values are produced by using the 'co_yield' keyword and the coroutine can
 * also consume other asynchronous operations using the 'co_await' keyword.
 * The end of the sequence is indicated by executing 'co_return;' either
 * explicitly or by letting execution run off the end of the coroutine.
 *
 * Reference Type
 * --------------
 * The first template parameter controls the 'reference' type.
 * i.e. the type returned when you dereference the iterator using operator*().
 * This type is typically specified as an actual reference type.
 * eg. 'const T&' (non-mutable), 'T&' (mutable)  or 'T&&' (movable) depending
 * what access you want your consumers to have to the yielded values.
 *
 * It's also possible to specify the 'Reference' template parameter as a value
 * type. In this case the generator takes a copy of the yielded value (either
 * copied or move-constructed) and you get a copy of this value every time
 * you dereference the iterator with '*iter'.
 * This can be expensive for types that are expensive to copy, but can provide
 * a small performance win for types that are cheap to copy (like built-in
 * integer types).
 *
 * Value Type
 * ----------
 * The second template parameter is optional, but if specified can be used as
 * the value-type that should be used to take a copy of the value returned by
 * the Reference type.
 * By default this type is the same as 'Reference' type stripped of qualifiers
 * and references. However, in some cases it can be a different type.
 * For example, if the 'Reference' type was a non-reference proxy type.
 *
 * Example:
 *
 *     AsyncGenerator<std::tuple<const K&, V&>, std::tuple<K, V>> getItems() {
 *         auto firstMap = co_await getFirstMap();
 *         for (auto&& [k, v] : firstMap) {
 *           co_yield {k, v};
 *         }
 *         auto secondMap = co_await getSecondMap();
 *         for (auto&& [k, v] : secondMap) {
 *           co_yield {k, v};
 *         }
 *      }
 *
 * This is mostly useful for generic algorithms that need to take copies of
 * elements of the sequence.
 *
 * Executor Affinity
 * -----------------
 * An AsyncGenerator coroutine has similar executor-affinity to that of the
 * folly::coro::Task coroutine type. Every time a consumer requests a new value
 * from the generator using 'co_await ++it' the generator inherits the caller's
 * current executor. The coroutine will ensure that it always resumes on the
 * associated executor when resuming from `co_await' expression until it hits
 * the next 'co_yield' or 'co_return' statement.
 * Note that the executor can potentially change at a 'co_yield' statement if
 * the next element of the sequence is requested from a consumer coroutine that
 * is associated with a different executor.
 *
 * Example: Writing an async generator.
 *
 *       folly::coro::AsyncGenerator<Record&&> getRecordsAsync() {
 *         auto resultSet = executeQuery(someQuery);
 *         for (;;) {
 *           auto resultSetPage = co_await resultSet.nextPage();
 *           if (resultSetPage.empty()) break;
 *           for (auto& row : resultSetPage) {
 *             co_yield Record{row.get("name"), row.get("email")};
 *           }
 *         }
 *  }
 *
 * Example: Consuming items from an async generator
 *
 *       folly::coro::Task<void> consumer() {
 *         auto records = getRecordsAsync();
 *         while (auto item = co_await records.next()) {
 *           auto&& record = *item;
 *           process(record);
 *         }
 *       }
 *
 * Async Cleanup
 * -------------
 * When the template parameter RequiresCleanup is true, the owner of an
 * AsyncGenerator is responsible for awaiting cleanup() before the generator
 * object's destructor is called. That allows to use folly::coro::co_scope_exit
 * awaitables inside AsyncGenerator, which are asynchronously executed when
 * cleanup() is awaited. Note that the AsyncGenerator coroutine frame is
 * destroyed before co_scope_exit awaitables are executed.
 *
 * There is an alias CleanableAsyncGenerator for AsyncGenerator with
 * RequiresCleanup set to true.
 *
 * Drain safety
 * ------------
 * One significant difference between AsyncGenerator and folly::coro::Task is
 * that AsyncGenerator may be destroyed between next() calls - i.e. destroyed
 * without being fully drained.
 *
 * For example:
 *
 *   AsyncGenerator<int> gen() {
 *     SCOPE_EXIT {
 *       LOG(INFO) << "Step 4";
 *     };
 *     LOG(INFO) << "Step 1";
 *     co_yield 41;
 *     SCOPE_EXIT {
 *       LOG(INFO) << "Step 3";
 *     };
 *     LOG(INFO) << "Step 2";
 *     co_yield 42;
 *     SCOPE_EXIT {
 *       LOG(INFO) << "Never reached";
 *     };
 *     LOG(INFO) << "Never reached";
 *     co_yield 43;
 *   }
 *
 *   {
 *     AsyncGenerator<int> g = gen();
 *     while (auto next = co_await g.next()) {
 *       LOG(INFO) << *next;
 *       if (*next == 42) {
 *         break;
 *         // ^^^ this may trigger generator destruction before it is drained.
 *       }
 *     }
 *   }
 *
 * This means that when writing an AsyncGenerator, you should always document
 * whether such AsyncGenerator requires draining before destruction (drain
 * unsafe). When possible you should always aim to make AsyncGenerator not
 * require draining before destruction (drain safe).
 *
 * If an AsyncGenerator is drain unsafe, always mention this in the
 * documentation and ideally include some assertions that help detect cases
 * where such AsyncGenerator is destroyed without being fully drained.
 *
 * Example:
 *
 *   AsyncGenerator<int> gen() {
 *     auto drainGuard = makeGuard([] { LOG(FATAL) << "I shall be drained!"; });
 *     co_yield 41;
 *     co_yield 42;
 *     co_yield 43;
 *     drainGuard.dismiss();
 *   }
 */
template <
    typename Reference,
    typename Value = std::remove_cvref_t<Reference>,
    bool RequiresCleanup = false>
class [[nodiscard]] AsyncGenerator {
  static_assert(
      std::is_constructible<Value, Reference>::value,
      "AsyncGenerator 'value_type' must be constructible from a 'reference'.");

 public:
  using promise_type =
      detail::AsyncGeneratorPromise<Reference, Value, RequiresCleanup>;
  // Standard `AsyncGenerator` coros can easily capture references & other
  // unsafe aliasing.
  //
  // Future: Implement a `coro/safe` generator wrapper, like
  // `async_closure_gen`.
  template <safe_alias>
  using folly_private_safe_alias_t = safe_alias_constant<safe_alias::unsafe>;

 private:
  using handle_t = coroutine_handle<promise_type>;

 public:
  using value_type = Value;
  using reference = Reference;
  using pointer = std::add_pointer_t<Reference>;

 public:
  AsyncGenerator() noexcept : coro_() {}

  AsyncGenerator(AsyncGenerator&& other) noexcept
      : coro_(std::exchange(other.coro_, {})) {}

  ~AsyncGenerator() {
    if (coro_) {
      if constexpr (RequiresCleanup) {
        LOG(FATAL) << "cleanup() hasn't been called!";
      }

      coro_.destroy();
    }
  }

  class CleanupSemiAwaitable;

  class [[nodiscard]] CleanupAwaitable {
   public:
    struct Awaiter {
      bool await_ready() noexcept { return !scopeExit_; }

      template <typename Promise>
      FOLLY_NOINLINE auto await_suspend(
          coroutine_handle<Promise> continuation) noexcept {
        asyncFrame_.setReturnAddress();
        scopeExit_.promise().setContext(
            continuation, &asyncFrame_, executor_.get_alias());
        if constexpr (detail::promiseHasAsyncFrame_v<Promise>) {
          folly::pushAsyncStackFrameCallerCallee(
              continuation.promise().getAsyncFrame(), asyncFrame_);
          return scopeExit_;
        } else {
          folly::resumeCoroutineWithNewAsyncStackRoot(scopeExit_);
        }
      }

      void await_resume() noexcept {}

      coroutine_handle<detail::ScopeExitTaskPromiseBase> scopeExit_;
      folly::Executor::KeepAlive<> executor_;
      folly::AsyncStackFrame asyncFrame_;
    };

    Awaiter operator co_await() && noexcept {
      return Awaiter{scopeExit_, std::move(executor_), {}};
    }

   private:
    friend CleanupSemiAwaitable;

    CleanupAwaitable(
        coroutine_handle<detail::ScopeExitTaskPromiseBase> scopeExit,
        folly::Executor::KeepAlive<> executor) noexcept
        : scopeExit_{scopeExit}, executor_{std::move(executor)} {}

    friend CleanupAwaitable tag_invoke(
        cpo_t<co_withAsyncStack>, CleanupAwaitable awaitable) noexcept {
      return std::move(awaitable);
    }

    coroutine_handle<detail::ScopeExitTaskPromiseBase> scopeExit_;
    folly::Executor::KeepAlive<> executor_;
  };

  class [[nodiscard]] CleanupSemiAwaitable {
   public:
    CleanupAwaitable viaIfAsync(Executor::KeepAlive<> executor) noexcept {
      return CleanupAwaitable{scopeExit_, std::move(executor)};
    }

    template <safe_alias>
    using folly_private_safe_alias_t = safe_alias_constant<safe_alias::unsafe>;

   private:
    friend AsyncGenerator;

    explicit CleanupSemiAwaitable(
        coroutine_handle<detail::ScopeExitTaskPromiseBase> scopeExit) noexcept
        : scopeExit_{scopeExit} {}

    coroutine_handle<detail::ScopeExitTaskPromiseBase> scopeExit_;
  };

  CleanupSemiAwaitable cleanup() && {
    static_assert(RequiresCleanup);
    if (coro_) {
      SCOPE_EXIT {
        std::exchange(coro_, {}).destroy();
      };
      return CleanupSemiAwaitable{coro_.promise().scopeExit_};
    } else {
      return CleanupSemiAwaitable{{}};
    }
  }

  AsyncGenerator& operator=(AsyncGenerator&& other) noexcept {
    auto oldCoro = std::exchange(coro_, std::exchange(other.coro_, {}));
    if (oldCoro) {
      CHECK(!RequiresCleanup) << "cleanup() hasn't been called!";
      oldCoro.destroy();
    }
    return *this;
  }

  void swap(AsyncGenerator& other) noexcept { std::swap(coro_, other.coro_); }

  class NextAwaitable;
  class NextSemiAwaitable;

  class NextResult {
   public:
    NextResult() noexcept : hasValue_(false) {}

    NextResult(NextResult&& other) noexcept : hasValue_(other.hasValue_) {
      if (hasValue_) {
        value_.construct(std::move(other.value_).get());
      }
    }

    ~NextResult() {
      if (hasValue_) {
        value_.destruct();
      }
    }

    NextResult& operator=(NextResult&& other) {
      if (&other != this) {
        if (has_value()) {
          hasValue_ = false;
          value_.destruct();
        }

        if (other.has_value()) {
          value_.construct(std::move(other.value_).get());
          hasValue_ = true;
        }
      }
      return *this;
    }

    bool has_value() const noexcept { return hasValue_; }

    explicit operator bool() const noexcept { return has_value(); }

    decltype(auto) value() & {
      DCHECK(has_value());
      return value_.get();
    }

    decltype(auto) value() && {
      DCHECK(has_value());
      return std::move(value_).get();
    }

    decltype(auto) value() const& {
      DCHECK(has_value());
      return value_.get();
    }

    decltype(auto) value() const&& {
      DCHECK(has_value());
      return std::move(value_).get();
    }

    decltype(auto) operator*() & { return value(); }

    decltype(auto) operator*() && { return std::move(*this).value(); }

    decltype(auto) operator*() const& { return value(); }

    decltype(auto) operator*() const&& { return std::move(*this).value(); }

    decltype(auto) operator->() {
      DCHECK(has_value());
      auto&& x = value_.get();
      return std::addressof(x);
    }

    decltype(auto) operator->() const {
      DCHECK(has_value());
      auto&& x = value_.get();
      return std::addressof(x);
    }

   private:
    friend NextAwaitable;
    explicit NextResult(handle_t coro) noexcept : hasValue_(true) {
      value_.construct(coro.promise().getRvalue());
    }

    detail::ManualLifetime<Reference> value_;
    bool hasValue_ = false;
  };

  class NextAwaitable {
   public:
    bool await_ready() noexcept { return !coro_; }

    template <typename Promise>
    FOLLY_NOINLINE auto await_suspend(
        coroutine_handle<Promise> continuation) noexcept {
      auto& promise = coro_.promise();

      promise.setContinuation(continuation);
      promise.clearValue();

      auto& asyncFrame = promise.getAsyncFrame();
      asyncFrame.setReturnAddress();

      if constexpr (detail::promiseHasAsyncFrame_v<Promise>) {
        folly::pushAsyncStackFrameCallerCallee(
            continuation.promise().getAsyncFrame(), asyncFrame);
        return coro_;
      } else {
        folly::resumeCoroutineWithNewAsyncStackRoot(coro_);
      }
    }

    NextResult await_resume() {
      if (!coro_) {
        return NextResult{};
      } else if (!coro_.promise().hasValue()) {
        coro_.promise().throwIfException();
        return NextResult{};
      } else {
        return NextResult{coro_};
      }
    }

    folly::Try<NextResult> await_resume_try() noexcept {
      if (coro_) {
        if (coro_.promise().hasValue()) {
          return folly::Try<NextResult>(NextResult{coro_});
        } else if (coro_.promise().hasException()) {
          return folly::Try<NextResult>(
              std::move(coro_.promise().getException()));
        }
      }
      return folly::Try<NextResult>(NextResult{});
    }

   private:
    friend NextSemiAwaitable;
    explicit NextAwaitable(handle_t coro) noexcept : coro_(coro) {}

    friend NextAwaitable tag_invoke(
        cpo_t<co_withAsyncStack>, NextAwaitable awaitable) noexcept {
      return NextAwaitable{awaitable.coro_};
    }

    handle_t coro_;
  };

  class NextSemiAwaitable {
   public:
    NextAwaitable viaIfAsync(Executor::KeepAlive<> executor) noexcept {
      if (coro_) {
        coro_.promise().setExecutor(std::move(executor));
      }
      return NextAwaitable{coro_};
    }

    friend NextSemiAwaitable co_withCancellation(
        const CancellationToken& cancelToken, NextSemiAwaitable&& awaitable) {
      if (awaitable.coro_) {
        awaitable.coro_.promise().setCancellationToken(cancelToken);
      }
      return NextSemiAwaitable{std::exchange(awaitable.coro_, {})};
    }

    friend NextSemiAwaitable co_withCancellation(
        CancellationToken&& cancelToken, NextSemiAwaitable&& awaitable) {
      auto consumedToken = std::move(cancelToken);
      if (awaitable.coro_) {
        awaitable.coro_.promise().setCancellationToken(
            std::move(consumedToken));
      }
      return NextSemiAwaitable{std::exchange(awaitable.coro_, {})};
    }

    template <safe_alias>
    using folly_private_safe_alias_t = safe_alias_constant<safe_alias::unsafe>;

   private:
    friend AsyncGenerator;

    explicit NextSemiAwaitable(handle_t coro) noexcept : coro_(coro) {}

    handle_t coro_;
  };

  NextSemiAwaitable next() noexcept {
    DCHECK(!coro_ || !coro_.done());
    return NextSemiAwaitable{coro_};
  }

  template <typename F, typename... A, typename F_, typename... A_>
  friend AsyncGenerator tag_invoke(
      tag_t<co_invoke_fn>, tag_t<AsyncGenerator, F, A...>, F_ f, A_... a) {
    if constexpr (RequiresCleanup) {
      auto&& [fScoped, r] = co_await co_scope_exit(
          [](auto&&, auto&& gen) { return std::move(gen).cleanup(); },
          static_cast<F&&>(f),
          AsyncGenerator{});
      r = std::invoke(static_cast<F&&>(fScoped), static_cast<A&&>(a)...);
      while (true) {
        co_yield co_result(co_await co_awaitTry(r.next()));
      }
    } else {
      auto r = std::invoke(static_cast<F&&>(f), static_cast<A&&>(a)...);
      while (true) {
        co_yield co_result(co_await co_awaitTry(r.next()));
      }
    }
  }

 private:
  friend promise_type;

  explicit AsyncGenerator(coroutine_handle<promise_type> coro) noexcept
      : coro_(coro) {}

  coroutine_handle<promise_type> coro_;
};

template <typename Reference, typename Value = std::remove_cvref_t<Reference>>
using CleanableAsyncGenerator =
    AsyncGenerator<Reference, Value, true /* RequiresCleanup */>;

namespace detail {

template <bool RequiresCleanup>
struct BaseAsyncGeneratorPromise {};

template <>
struct BaseAsyncGeneratorPromise<true> {
  coroutine_handle<ScopeExitTaskPromiseBase> scopeExit_;
};

enum class AsyncGeneratorPromiseState : std::uint8_t {
  INVALID,
  VALUE,
  EXCEPTION_WRAPPER,
  DONE,
};

template <
    typename Reference,
    typename Value,
    bool RequiresCleanup /* = false, in BasePromise.h */>
class AsyncGeneratorPromise final
    : public ExtendedCoroutinePromiseCrtp<
          AsyncGeneratorPromise<Reference, Value, RequiresCleanup>>,
      BaseAsyncGeneratorPromise<RequiresCleanup>,
      public BasePromise<AsyncGeneratorPromiseState> {
  class YieldAwaiter {
   public:
    bool await_ready() noexcept { return false; }
    coroutine_handle<> await_suspend_promise(
        AsyncGeneratorPromise& promise) noexcept {
      // Pop AsyncStackFrame first as clearContext() clears the frame state.
      folly::popAsyncStackFrameCallee(promise.getAsyncFrame());
      promise.clearContext();
      if (promise.hasException()) {
        auto [handle, frame] =
            promise.continuation_.getErrorHandle(promise.getException());
        return handle.getHandle();
      }
      return promise.continuation_.getHandle();
    }
    coroutine_handle<> await_suspend(
        coroutine_handle<AsyncGeneratorPromise> h) noexcept {
      return await_suspend_promise(h.promise());
    }
    void await_resume() noexcept {}
  };

  using State = AsyncGeneratorPromiseState;
  State state() const { return tailStorage_; }
  void setState(State s) { tailStorage_ = s; }

 public:
  template <typename... Args>
  AsyncGeneratorPromise(Args&... args) {
    setState(State::INVALID);
    if constexpr (RequiresCleanup) {
      scheduleAutoCleanupIfNeeded(
          coroutine_handle<AsyncGeneratorPromise>::from_promise(*this),
          args...);
    }
  }

  ~AsyncGeneratorPromise() {
    switch (state()) {
      case State::VALUE:
        folly::coro::detail::deactivate(value_);
        break;
      case State::EXCEPTION_WRAPPER:
        folly::coro::detail::deactivate(exceptionWrapper_);
        break;
      case State::DONE:
      case State::INVALID:
        break;
    }
  }

  FOLLY_CORO_DEFINE_FRAME_ALLOCATOR(
      ::folly::coro::detail::CoroFrameAsyncAllocator)

  AsyncGenerator<Reference, Value, RequiresCleanup>
  get_return_object() noexcept {
    return AsyncGenerator<Reference, Value, RequiresCleanup>{
        coroutine_handle<AsyncGeneratorPromise>::from_promise(*this)};
  }

  suspend_always initial_suspend() noexcept { return {}; }

  YieldAwaiter final_suspend() noexcept {
    DCHECK(!hasValue());
    return {};
  }

  YieldAwaiter yield_value(Reference&& value) noexcept(
      std::is_nothrow_move_constructible<Reference>::value) {
    DCHECK(state() == State::INVALID);
    folly::coro::detail::activate(value_, static_cast<Reference&&>(value));
    setState(State::VALUE);
    return YieldAwaiter{};
  }

  /// In the case where 'Reference' is not actually a reference-type we
  /// allow implicit conversion from the co_yield argument to Reference.
  /// However, we don't want to allow this for cases where 'Reference' _is_
  /// a reference because this could result in the reference binding to a
  /// temporary that results from an implicit conversion.
  template <
      typename U,
      std::enable_if_t<
          !std::is_reference_v<Reference> &&
              std::is_convertible_v<U&&, Reference>,
          int> = 0>
  YieldAwaiter yield_value(U&& value) noexcept(
      std::is_nothrow_constructible_v<Reference, U>) {
    DCHECK(state() == State::INVALID);
    folly::coro::detail::activate(value_, static_cast<U&&>(value));
    setState(State::VALUE);
    return {};
  }

  YieldAwaiter yield_value(co_error&& error) noexcept {
    DCHECK(state() == State::INVALID);
    folly::coro::detail::activate(
        exceptionWrapper_, std::move(error.exception()));
    setState(State::EXCEPTION_WRAPPER);
    return {};
  }

  template <typename ContainerRef>
  YieldAwaiter yield_value(co_result<Value, ContainerRef> res) {
    return std::move(res).consumeIntoGenerator(
        [this](auto&& value) {
          return this->yield_value(static_cast<decltype(value)&&>(value));
        },
        [this](exception_wrapper ex) {
          return this->yield_value(co_error(std::move(ex)));
        },
        [this] {
          this->return_void();
          return YieldAwaiter{};
        });
  }

  template <typename ContainerRef>
  YieldAwaiter yield_value(
      co_result<
          typename AsyncGenerator<Reference, Value, RequiresCleanup>::
              NextResult,
          ContainerRef> res) {
    return std::move(res).consumeIntoGenerator(
        [this](auto&& result) {
          if (result.has_value()) {
            return this->yield_value(std::move(result.value()));
          } else {
            this->return_void();
            return YieldAwaiter{};
          }
        },
        [this](exception_wrapper ex) {
          return this->yield_value(co_error(std::move(ex)));
        },
        [this] {
          return this->yield_value(co_error(UsingUninitializedTry{}));
        });
  }

  using BasePromise<AsyncGeneratorPromiseState>::await_transform;

  variant_awaitable<YieldAwaiter, ready_awaitable<>> await_transform(
      co_safe_point_t) noexcept {
    return do_safe_point<YieldAwaiter>(*this);
  }

  void unhandled_exception() noexcept {
    DCHECK(state() == State::INVALID);
    folly::coro::detail::activate(exceptionWrapper_, current_exception());
    setState(State::EXCEPTION_WRAPPER);
  }

  void return_void() noexcept {
    DCHECK(state() == State::INVALID);
    setState(State::DONE);
  }

  void setExecutor(folly::Executor::KeepAlive<> executor) noexcept {
    DCHECK(executor);
    executor_ = std::move(executor);
  }

  void setContinuation(ExtendedCoroutineHandle continuation) noexcept {
    continuation_ = continuation;
  }

  bool hasException() const noexcept {
    return state() == State::EXCEPTION_WRAPPER;
  }

  folly::exception_wrapper& getException() noexcept {
    DCHECK(hasException());
    return exceptionWrapper_.get();
  }

  void throwIfException() {
    if (state() == State::EXCEPTION_WRAPPER) {
      exceptionWrapper_.get().throw_exception();
    }
  }

  decltype(auto) getRvalue() noexcept {
    DCHECK(hasValue());
    return std::move(value_).get();
  }

  void clearValue() noexcept {
    if (hasValue()) {
      setState(State::INVALID);
      folly::coro::detail::deactivate(value_);
    } else {
      CHECK(state() != State::DONE)
          << "Using generator after receiving completion.";
      CHECK(state() != State::EXCEPTION_WRAPPER)
          << "Using generator after receiving exception.";
    }
  }

  bool hasValue() const noexcept { return state() == State::VALUE; }

  // Unlike `getErrorHandleUncheckedImpl`, checks the type of `me`.
  static std::optional<ExtendedCoroutineHandle::ErrorHandle> getErrorHandleImpl(
      AsyncGeneratorPromise& me, exception_wrapper& ex) {
    return getErrorHandleUncheckedImpl(me, ex);
  }

 private:
  friend AsyncGenerator<Reference, Value, RequiresCleanup>;

  void clearContext() noexcept {
    executor_ = {};
    cancelToken_ = {};
    hasCancelTokenOverride_ = false;
    asyncFrame_.clear();
  }

  friend coroutine_handle<ScopeExitTaskPromiseBase> tag_invoke(
      cpo_t<co_attachScopeExit>,
      AsyncGeneratorPromise& p,
      coroutine_handle<ScopeExitTaskPromiseBase> scopeExit) noexcept {
    static_assert(
        RequiresCleanup,
        "Only CleanableAsyncGenerator (AsyncGenerator with RequiresCleanup"
        " template parameter set to true) supports attaching co_scope_exit");
    return std::exchange(p.scopeExit_, scopeExit);
  }

  // From the base: continuation_, asyncFrame_, executor_, cancelToken_,
  // hasCancelTokenOverride_, tailStorage_ == state() / setState()
  union {
    ManualLifetime<folly::exception_wrapper> exceptionWrapper_;
    ManualLifetime<Reference> value_;
  };
};

} // namespace detail

template <typename Reference, typename Value>
auto tag_invoke(
    cpo_t<co_cleanup>, CleanableAsyncGenerator<Reference, Value>&& gen) {
  return std::move(gen).cleanup();
}

} // namespace coro

} // namespace folly

#endif
