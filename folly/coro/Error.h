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

// KEEP THIS HEADER MINIMAL -- it is included by all `result` coro users.

#include <cassert>
#include <type_traits>

#include <folly/ExceptionWrapper.h>
#include <folly/OperationCancelled.h>

namespace folly::coro {

class co_error final {
 public:
  template <typename... A>
    requires(
        sizeof...(A) > 0 && std::is_constructible_v<exception_wrapper, A...>)
  explicit co_error(A&&... a) noexcept(
      std::is_nothrow_constructible<exception_wrapper, A...>::value)
      : ex_(static_cast<A&&>(a)...) {
    static_assert(
        sizeof...(A) != 1 ||
            !(std::is_same_v<std::decay_t<A>, OperationCancelled> && ...),
        "Use co_stopped_may_throw instead of co_error(OperationCancelled{})");
    assert(ex_);
  }

  const exception_wrapper& exception() const { return ex_; }

  exception_wrapper& exception() { return ex_; }

 private:
  exception_wrapper ex_;
};

class co_stopped_may_throw_t final {
 public:
  /* implicit */ operator co_error() const {
    return co_error(make_exception_wrapper<OperationCancelled>());
  }
};

inline constexpr co_stopped_may_throw_t co_stopped_may_throw{};

[[deprecated("Use co_stopped_may_throw")]]
inline constexpr co_stopped_may_throw_t co_cancelled{};

} // namespace folly::coro
