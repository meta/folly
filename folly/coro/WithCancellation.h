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

#include <folly/CancellationToken.h>
#include <folly/coro/Coroutine.h>
#include <folly/lang/CustomizationPoint.h>
#include <folly/lang/MustUseImmediately.h>

#if FOLLY_HAS_COROUTINES

/**
 * \file coro/WithCancellation.h
 * co_withCancellation allows caller to pass in a cancellation token to a
 * awaitable
 *
 * \refcode folly/docs/examples/folly/coro/WithCancellation.cpp
 */

namespace folly {
namespace coro {

namespace detail {
namespace adl {

/// Default implementation that does not hook the cancellation token.
/// Types must opt-in to hooking cancellation by customising this function.
template <typename Awaitable>
  requires(!folly::ext::must_use_immediately_v<Awaitable>)
// WART: The `Awaitable&&` overload must return `Awaitable&&` to support
// `Baton`s, which is immovable and awaitable only by lvalue.  We could remove
// this, and simplify other code, by migrating `Baton`s to emit an explicit
// must-use-immediately awaitable instead of awaiting them by lvalue.
Awaitable&& co_withCancellation(
    const folly::CancellationToken&, Awaitable&& awaitable) noexcept {
  return static_cast<Awaitable&&>(awaitable);
}
template <typename Awaitable>
  requires folly::ext::must_use_immediately_v<Awaitable>
Awaitable co_withCancellation(
    const folly::CancellationToken&, Awaitable awaitable) noexcept {
  return folly::ext::must_use_immediately_unsafe_mover(std::move(awaitable))();
}

struct WithCancellationFunction {
  template <typename Awaitable>
    requires(!folly::ext::must_use_immediately_v<Awaitable>)
  auto operator()(
      const folly::CancellationToken& cancelToken, Awaitable&& awaitable) const
      noexcept(noexcept(co_withCancellation(
          cancelToken, static_cast<Awaitable&&>(awaitable))))
          -> decltype(co_withCancellation(
              cancelToken, static_cast<Awaitable&&>(awaitable))) {
    return co_withCancellation(
        cancelToken, static_cast<Awaitable&&>(awaitable));
  }
  template <typename Awaitable>
    requires folly::ext::must_use_immediately_v<Awaitable>
  auto operator()(
      const folly::CancellationToken& cancelToken, Awaitable awaitable) const
      noexcept(noexcept(co_withCancellation(
          cancelToken,
          folly::ext::must_use_immediately_unsafe_mover(
              std::move(awaitable))())))
          -> decltype(co_withCancellation(
              cancelToken,
              folly::ext::must_use_immediately_unsafe_mover(
                  std::move(awaitable))())) {
    return co_withCancellation(
        cancelToken,
        folly::ext::must_use_immediately_unsafe_mover(std::move(awaitable))());
  }
  template <typename Awaitable>
    requires(!folly::ext::must_use_immediately_v<Awaitable>)
  auto operator()(folly::CancellationToken&& cancelToken, Awaitable&& awaitable)
      const noexcept(noexcept(co_withCancellation(
          std::move(cancelToken), static_cast<Awaitable&&>(awaitable))))
          -> decltype(co_withCancellation(
              std::move(cancelToken), static_cast<Awaitable&&>(awaitable))) {
    return co_withCancellation(
        std::move(cancelToken), static_cast<Awaitable&&>(awaitable));
  }
  template <typename Awaitable>
    requires folly::ext::must_use_immediately_v<Awaitable>
  auto operator()(folly::CancellationToken&& cancelToken, Awaitable awaitable)
      const noexcept(noexcept(co_withCancellation(
          std::move(cancelToken),
          folly::ext::must_use_immediately_unsafe_mover(
              std::move(awaitable))())))
          -> decltype(co_withCancellation(
              std::move(cancelToken),
              folly::ext::must_use_immediately_unsafe_mover(
                  std::move(awaitable))())) {
    return co_withCancellation(
        std::move(cancelToken),
        folly::ext::must_use_immediately_unsafe_mover(std::move(awaitable))());
  }
};
} // namespace adl
} // namespace detail

FOLLY_DEFINE_CPO(detail::adl::WithCancellationFunction, co_withCancellation)

} // namespace coro
} // namespace folly

#endif // FOLLY_HAS_COROUTINES
