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

#include <folly/Utility.h>
#include <folly/coro/Coroutine.h> // delete with clang-15 support
#include <folly/coro/detail/Malloc.h>
#include <folly/result/result.h>

#if FOLLY_HAS_RESULT

#include <coroutine>

namespace folly {
namespace detail {

template <typename>
struct result_promise_base;

template <typename T>
class result_promise_return {
 private:
  result<T> storage_{typename result<T>::has_value_sigil_t{}};
  result<T>*& ptr_;

 public:
  /* implicit */ result_promise_return(result_promise_base<T>& p) noexcept
      : ptr_{p.value_} {
    ptr_ = &storage_;
  }
  result_promise_return(result_promise_return const&) = delete;
  void operator=(result_promise_return const&) = delete;
  result_promise_return(result_promise_return&&) = delete;
  void operator=(result_promise_return&&) = delete;
  // Remove this once clang 15 is well-forgotten. From D42260201:
  // letting dtor be trivial makes the coroutine crash
  ~result_promise_return() {}

  /* implicit */ operator result<T>() {
    // Simplify this once clang 15 is well-forgotten, and remove the dep on
    // `Coroutine.h`.  From D42260201: handle both deferred and eager
    // return-object conversion behaviors; see docs for
    // detect_promise_return_object_eager_conversion
    //
    // Storage is in "has value" sigil state with uninitialized `value_`;
    // `return_value` / `unhandled_exception` will initialize it.
    if (coro::detect_promise_return_object_eager_conversion()) { // eager
      return result<T>{typename result<T>::has_value_sigil_t{}, ptr_};
    } else { // deferred
      return std::move(storage_);
    }
  }
};

// (Defined in epitaph.cpp) Captures the current call stack and wraps the
// current exception with a stack epitaph, assigning the result into `eos`.
// Must be called from within a catch handler.
void stack_epitaph_for_unhandled_exception(error_or_stopped& eos) noexcept;

template <typename T>
struct result_promise_base {
  FOLLY_CORO_DEFINE_FRAME_ALLOCATOR_IF_SUPPORTED(
      ::folly::coro::detail::CoroFrameDefaultAllocator)

  result<T>* value_ = nullptr;

  result_promise_base() = default;
  result_promise_base(result_promise_base const&) = delete;
  void operator=(result_promise_base const&) = delete;
  result_promise_base(result_promise_base&&) = delete;
  void operator=(result_promise_base&&) = delete;
  ~result_promise_base() = default;

  std::suspend_never initial_suspend() const noexcept { return {}; }
  std::suspend_never final_suspend() const noexcept { return {}; }
  // Always-inline so we don't have to skip this stack frame.  Tradeoff:
  // `coroFn` shows as having this line # due to DWARF limitations.
  //
  // Future: One way to improve this would be to `FOLLY_NOINLINE` this, and
  // skip the `unhandled_exception` frame iff it's not optimized away.
  FOLLY_ALWAYS_INLINE void unhandled_exception() noexcept {
    // Directly set `eos_` -- using `result` assignment would try to destroy
    // uninitialized `value_` (UB since we start in "has value" sigil state).
    //
    // Wrap the exception with a stack-trace epitaph so that the throw
    // location is visible when the error is logged.
    stack_epitaph_for_unhandled_exception(value_->eos_);
  }

  result_promise_return<T> get_return_object() noexcept { return *this; }

  // Only permit `or_unwind` awaitables.  See the rationale on
  // `result_or_unwind_crtp::or_unwind_awaitable`.
  template <typename U>
    requires requires { typename std::remove_cvref_t<U>::or_unwind_awaitable; }
  U&& await_transform(U&& u) noexcept {
    return static_cast<U&&>(u);
  }
};

template <typename T>
  requires(!std::is_void_v<T>)
struct result_promise<T, void> : public result_promise_base<T> {
  // For reference types, this deliberately requires users to `co_return`
  // one of `std::ref`, `std::cref`, or `folly::rref`.
  //
  // The default for `U` is tested in `returnImplicitCtor`.
  template <typename U = T>
  void return_value(U&& u) {
    // We're in "has value" sigil state with uninitialized `value_`.  Use
    // placement `new` -- NEVER assignment, which would call `operator=(T&)` on
    // garbage (UB!).  This supports non-default-constructible `T`, helps perf.
    auto& v = *this->value_;
    if constexpr (std::is_same_v<std::remove_cvref_t<U>, error_or_stopped>) {
      v.eos_ = static_cast<U&&>(u);
    } else if constexpr (std::is_same_v<std::remove_cvref_t<U>, result<T>>) {
      // Returning `result<T>` (with `T` matching ours) acts like `or_unwind()`:
      if (static_cast<U&&>(u).has_value()) {
        return_value(static_cast<U&&>(u).value_or_throw());
      } else {
        // NOLINTNEXTLINE(facebook-hte-MissingStdForward): false positive
        return_value(forward_like<U>(u.eos_));
      }
    } else {
      // Returning a value; `eos_` already says "has value"
      new (&v.value_)
          typename std::remove_reference_t<decltype(v)>::storage_type(
              static_cast<U&&>(u));
    }
  }
};

template <typename T>
  requires std::is_void_v<T>
struct result_promise<T, void> : public result_promise_base<T> {
  // You can fail via `co_await err` since void coros only allow `co_return;`.
  // NB: `has_value_sigil_t` sets `eos_` to "has value", nothing to do.
  void return_void() {}
};

template <typename T> // Need an alias since nested types cannot be deduced.
using result_promise_handle = std::coroutine_handle<result_promise<T>>;

} // namespace detail
} // namespace folly

#endif // FOLLY_HAS_RESULT
