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

#include <optional>
#include <type_traits>
#include <variant>

#include <folly/Portability.h>
#include <folly/Utility.h>

#if FOLLY_HAS_COROUTINES
#include <coroutine>
#endif

//  A place for foundational vocabulary types.
//
//  This header reexports the foundational vocabulary coroutine-helper types
//  from the standard, and exports several new foundational vocabulary types
//  as well.
//
//  Types which are non-foundational and non-vocabulary should go elsewhere.

#if FOLLY_HAS_COROUTINES

namespace folly {
class exception_wrapper;
struct AsyncStackFrame;
} // namespace folly

namespace folly::coro {

using std::coroutine_handle;
using std::coroutine_traits;
using std::noop_coroutine;
using std::noop_coroutine_handle;
using std::noop_coroutine_promise;
using std::suspend_always;
using std::suspend_never;

//  ready_awaitable
//
//  An awaitable which is immediately ready with a value. Suspension is no-op.
//  Resumption returns the value.
//
//  The value type is permitted to be a reference.
template <typename T = void>
class ready_awaitable {
  static_assert(!std::is_void<T>::value, "base template unsuitable for void");

 public:
  explicit ready_awaitable(T value) //
      noexcept(noexcept(T(FOLLY_DECLVAL(T&&))))
      : value_(static_cast<T&&>(value)) {}

  bool await_ready() noexcept { return true; }
  void await_suspend(coroutine_handle<>) noexcept {}
  T await_resume() noexcept(noexcept(T(FOLLY_DECLVAL(T&&)))) {
    return static_cast<T&&>(value_);
  }

 private:
  T value_;
};

//  ready_awaitable
//
//  An awaitable type which is immediately ready. Suspension is a no-op.
template <>
class ready_awaitable<void> {
 public:
  ready_awaitable() noexcept = default;

  bool await_ready() noexcept { return true; }
  void await_suspend(coroutine_handle<>) noexcept {}
  void await_resume() noexcept {}
};

namespace detail {

//  await_suspend_return_coroutine_fn
//  await_suspend_return_coroutine
//
//  The special member await_suspend has three forms, differing in their return
//  types. It may return void, bool, or coroutine_handle<>. This invokes member
//  await_suspend on the argument, conspiring always to return coroutine_handle
//  no matter the underlying form of member await_suspend on the argument.
struct await_suspend_return_coroutine_fn {
  template <typename A, typename P>
  coroutine_handle<> operator()(A& a, coroutine_handle<P> coro) const
      noexcept(noexcept(a.await_suspend(coro))) {
    using result = decltype(a.await_suspend(coro));
    if constexpr (std::is_same<void, result>::value) {
      a.await_suspend(coro);
      return noop_coroutine();
    } else if constexpr (std::is_same<bool, result>::value) {
      return a.await_suspend(coro) ? noop_coroutine() : coro;
    } else {
      return a.await_suspend(coro);
    }
  }
};
inline constexpr await_suspend_return_coroutine_fn
    await_suspend_return_coroutine{};

} // namespace detail

#if __has_include(<variant>)

//  variant_awaitable
//
//  An awaitable type which is backed by one of several possible underlying
//  awaitables.
template <typename... A>
class variant_awaitable : private std::variant<A...> {
 private:
  using base = std::variant<A...>;

  template <typename Visitor>
  auto visit(Visitor v) {
    return std::visit(v, static_cast<base&>(*this));
  }

 public:
  // imports the base-class constructors wholesale for implementation simplicity
  using base::base; // assume there are no valueless-by-exception instances

  auto await_ready() noexcept(
      (noexcept(FOLLY_DECLVAL(A&).await_ready()) && ...)) {
    return visit([&](auto& a) { return a.await_ready(); });
  }
  template <typename P>
  auto await_suspend(coroutine_handle<P> coro) noexcept(
      (noexcept(FOLLY_DECLVAL(A&).await_suspend(coro)) && ...)) {
    auto impl = detail::await_suspend_return_coroutine;
    return visit([&](auto& a) { return impl(a, coro); });
  }
  auto await_resume() noexcept(
      (noexcept(FOLLY_DECLVAL(A&).await_resume()) && ...)) {
    return visit([&](auto& a) { return a.await_resume(); });
  }
};

#endif // __has_include(<variant>)

//  ----

namespace detail {

struct detect_promise_return_object_eager_conversion_ {
  struct promise_type {
    struct return_object {
      /* implicit */ return_object(promise_type& p) noexcept : promise{&p} {
        promise->object = this;
      }
      ~return_object() {
        if (promise) {
          promise->object = nullptr;
        }
      }

      promise_type* promise;
      bool body_ran = false;
    };

    ~promise_type() {
      if (object) {
        object->promise = nullptr;
      }
    }

    suspend_never initial_suspend() const noexcept { return {}; }
    suspend_never final_suspend() const noexcept { return {}; }
    void unhandled_exception() {}

    return_object get_return_object() noexcept { return {*this}; }
    //  Record that the coroutine body has run. Guarded on `object` so that, in
    //  the eager case where the return-object temporary is already destroyed
    //  (it nulls `object` in its destructor), this is a safe no-op.
    void return_void() {
      if (object) {
        object->body_ran = true;
      }
    }

    return_object* object = nullptr;
  };

  //  Conversion is eager iff it happens before the coroutine body runs. This is
  //  measured directly via `body_ran` rather than via promise liveness: some
  //  compilers (clang >= 22) destroy the promise after the return-object
  //  conversion rather than before, so the promise may still be alive at
  //  conversion time even when conversion is deferred.
  /* implicit */ detect_promise_return_object_eager_conversion_(
      promise_type::return_object const& o) noexcept
      : eager{!o.body_ran} {}
  //  letting the coroutine type be trivially-copyable makes the coroutine crash
  //  under clang; to work around, provide an empty but not trivial destructor
  ~detect_promise_return_object_eager_conversion_() {}

  bool eager = false;

  static detect_promise_return_object_eager_conversion_ go() noexcept {
    co_return;
  }
};

} // namespace detail

//  detect_promise_return_object_eager_conversion
//
//  Returns true if the compiler implements coroutine promise return-object
//  conversion eagerly and returns false if the compiler defers conversion.
//
//  It is expected that the caller holds the promise return-object until the
//  promise is fulfilled, even when it is not the same type as the coroutine.
//
//    auto ret = promise.get_return_object();
//    initial-suspend, etc...
//    return ret;
//
//  But this expected behavior was, mistakenly, never specified.
//
//  Some compilers misbehave, where the caller holds precisely the coroutine
//  type by converting the promise return-object eagerly when it is of some
//  type different from the coroutine type.
//
//    coro-type ret = promise.get_return_object();
//    initial-suspend, etc...
//    return ret;
//
//  Known behaviors are as follows:
//  * For msvc, conversion is eager for vs < 2019 update 16.5 (msc ver 1925) and
//    is deferred for vs >= 2019 update 16.5 (msc ver 1925).
//    References:
//      https://developercommunity.visualstudio.com/t/c-coroutine-get-return-object-converted-too-early/222420
//  * For g++, conversion is deferred.
//  * For clang++, conversion is eager for 15 <= llvm < 17 and is deferred for
//    llvm < 15 or llvm >= 17.
//    References:
//      https://reviews.llvm.org/D117087
//      https://github.com/llvm/llvm-project/issues/56532
//      https://reviews.llvm.org/D145639
//
//  Meta sometimes uses llvm patched to have deferred conversion where the
//  corresponding upstream implements eager conversion. So version numbers do
//  not tell the whole story.
//
//  This function detects which behavior the compiler implements at a mix of
//  compile time and run time, depending on the compiler. It is only necessary
//  to do the runtime detection for llvm but, conveniently, llvm is able to do
//  full heap-allocation elision ("HALO") and optimize the detection down to a
//  constant.
//
//  TODO: Remove this detection once the behavior is specified.
inline bool detect_promise_return_object_eager_conversion() {
  using coro = detail::detect_promise_return_object_eager_conversion_;
  constexpr auto t = kMscVer && kMscVer < 1925;
  constexpr auto f = (kGnuc && !kIsClang) || (kMscVer >= 1925);
  return t ? true : f ? false : coro::go().eager;
}

template <typename>
class ExtendedCoroutinePromiseCrtp;

namespace detail {
template <typename, typename, typename>
class TaskPromiseWrapperBase;
}

// Extended version of coroutine_handle<void>
// Assumes (and enforces) assumption that coroutine_handle is a pointer
class ExtendedCoroutineHandle {
 protected:
  template <typename>
  friend class ExtendedCoroutinePromiseCrtp;
  template <typename, typename, typename>
  friend class detail::TaskPromiseWrapperBase;
  // This passkey aims to stop end users from calling `getPromiseBase`, which
  // is an unsafe implementation detail, and to prevent overload ambiguity.
  //
  // It also doubles as the sigil for `use_extended_handle_concept`, another
  // private detail.
  class PrivateTag {
   private:
    friend ExtendedCoroutineHandle;
    PrivateTag() = default;
  };

 private:
  // SFINAE detection for the `use_extended_handle_concept` member type alias
  // that classes implementing `getErrorHandle` must expose.  We don't want to
  // use any kind of common base on `TaskWrapperPromise`, be it non-empty
  // `PromiseBase`, or a dedicated empty tag, since either one would break
  // empty-base optimization.

  template <typename T>
  using use_extended_handle_of_ = typename T::use_extended_handle_concept;

  template <typename T, typename Void = void>
  struct use_extended_handle {
    static_assert(
        require_sizeof<T>, "`use_extended_handle` on incomplete type");
    static constexpr bool value = false;
  };

  template <typename T>
  struct use_extended_handle<T, std::void_t<use_extended_handle_of_<T>>> {
    static constexpr bool value =
        std::is_same_v<use_extended_handle_of_<T>, PrivateTag>;
  };

 public:
  using ErrorHandle = std::pair<ExtendedCoroutineHandle, AsyncStackFrame*>;

  class PromiseBase {
   private:
    friend class ExtendedCoroutineHandle;
    template <typename>
    friend class ExtendedCoroutinePromiseCrtp;

    using Fn = std::optional<ErrorHandle>(PromiseBase*, exception_wrapper& ex);

    explicit PromiseBase(Fn* fn) : getErrorHandlePtr_(fn) {}
    ~PromiseBase() = default;

    // A manual vtable with 1 function. Benefits over virtual inheritance:
    //   - `TaskWrapperPromise` can implement `getErrorHandle` without bloating
    //     itself with with a vtable it does not need.
    //   - A tiny binary size win.
    //   - Derived classes like `TaskPromise` don't have to be `final` in order
    //     for the compiler to treat them as non-polymorphic.
    Fn* getErrorHandlePtr_;
  };

  template <typename Promise>
  /*implicit*/ ExtendedCoroutineHandle(
      coroutine_handle<Promise> handle) noexcept
      : basic_(handle), extended_(fromBasic(handle)) {}

  /*implicit*/ ExtendedCoroutineHandle(coroutine_handle<> handle) noexcept
      : basic_(handle) {}

  template <
      typename Promise,
      std::enable_if_t<use_extended_handle<Promise>::value, int> = 0>
  /*implicit*/ ExtendedCoroutineHandle(Promise* promise) noexcept
      : basic_(coroutine_handle<Promise>::from_promise(*promise)),
        extended_(Promise::getPromiseBase(PrivateTag{}, promise)) {}

  ExtendedCoroutineHandle() noexcept = default;

  void resume() { basic_.resume(); }

  void destroy() { basic_.destroy(); }

  coroutine_handle<> getHandle() const noexcept { return basic_; }

  ErrorHandle getErrorHandle(exception_wrapper& ex) {
    if (extended_) {
      if (auto res = extended_->getErrorHandlePtr_(extended_, ex)) {
        return *res;
      }
    }
    return {basic_, nullptr};
  }

  explicit operator bool() const noexcept { return !!basic_; }

 private:
  template <typename Promise>
  static auto fromBasic(coroutine_handle<Promise> handle) noexcept {
    if constexpr (use_extended_handle<Promise>::value) {
      return Promise::getPromiseBase(PrivateTag{}, &handle.promise());
    } else {
      return nullptr;
    }
  }

  coroutine_handle<> basic_;
  PromiseBase* extended_{nullptr};
};

// folly::coro types are expected to implement this extended promise interface.
//
// It allows types to provide a more efficient resumption path when they know
// they will be receiving an error result from the awaitee.
//
// First, publicly inherit from `ExtendedCoroutinePromiseCrtp<YourPromise>`,
// Second, implement this static method on `YourPromise`:
//
//   static std::optional<ExtendedCoroutineHandle::ErrorHandle>
//   getErrorHandleImpl(YourPromise&, exception_wrapper&);
//
// Return `std::nullopt` to avoid changing the resumption path.  Otherwise,
// return the `ExtendedCoroutineHandle` to resume & the active stack frame.
//
// DANGER: `YourPromise& promise` is a promise instance, but it might NOT
// directly correspond to a coro frame.  For example, if your coro is wrapped,
// that promise is a **member** inside a larger wrapper promise for the coro.
// Therefore, you must NOT call `coroutine_handle<...>::from_promise(promise)`.
// In the future, the true handle could be supplied, but none of the current
// coros required it.
template <typename Promise>
class ExtendedCoroutinePromiseCrtp
    : public ExtendedCoroutineHandle::PromiseBase {
 public:
  using use_extended_handle_concept = ExtendedCoroutineHandle::PrivateTag;

  static ExtendedCoroutineHandle::PromiseBase* getPromiseBase(
      ExtendedCoroutineHandle::PrivateTag, ExtendedCoroutinePromiseCrtp* me) {
    return me;
  }

 protected:
  using PromiseBase = typename ExtendedCoroutineHandle::PromiseBase;
  ExtendedCoroutinePromiseCrtp()
      : PromiseBase(+[](PromiseBase* p, exception_wrapper& ex) {
          return Promise::getErrorHandleImpl(*static_cast<Promise*>(p), ex);
        }) {}
  ~ExtendedCoroutinePromiseCrtp() = default;
};

} // namespace folly::coro

#endif
