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

#include <folly/functional/Invoke.h>

#if FOLLY_HAS_COROUTINES

namespace folly::coro {

/// A customization point that allows to provide an async cleanup function for a
/// type. folly::coro::AutoCleanup uses co_cleanup_fn as the default cleanup
/// function, so it is enough to define co_cleanup for a type to be able to use
/// it with AutoCleanup.
struct co_cleanup_fn {
  template <typename T>
    requires(
        folly::is_tag_invocable_v<co_cleanup_fn, T &&> &&
        !std::is_lvalue_reference_v<T>)
  auto operator()(T&& object) const
      noexcept(folly::is_nothrow_tag_invocable_v<co_cleanup_fn, T&&>)
          -> folly::tag_invoke_result_t<co_cleanup_fn, T&&> {
    return folly::tag_invoke(co_cleanup_fn{}, std::forward<T>(object));
  }

  template <typename T>
  void operator()(T& object) = delete;
};

FOLLY_DEFINE_CPO(co_cleanup_fn, co_cleanup)

} // namespace folly::coro

#endif
