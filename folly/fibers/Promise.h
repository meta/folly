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
#include <folly/Try.h>
#include <folly/fibers/traits.h>
#include <folly/functional/Invoke.h>

namespace folly {
namespace fibers {

class Baton;

template <typename T, typename BatonT = Baton>
class Promise {
 public:
  using value_type = T;
  using baton_type = BatonT;

  ~Promise();

  // not copyable
  Promise(const Promise&) = delete;
  Promise& operator=(const Promise&) = delete;

  // movable
  Promise(Promise&&) noexcept;
  Promise& operator=(Promise&&);

  /** Fulfill this promise (only for Promise<void>) */
  void setValue();

  /** Set the value (use perfect forwarding for both move and copy) */
  template <class M>
  void setValue(M&& value);

  /**
   * Fulfill the promise with a given try
   *
   * @param t A Try with either a value or an error.
   */
  void setTry(folly::Try<T>&& t);

  /** Fulfill this promise with the result of a function that takes no
    arguments and returns something implicitly convertible to T.
    Captures exceptions. e.g.

    p.setWith([] { do something that may throw; return a T; });
  */
  template <class F>
  void setWith(F&& func);

  /** Fulfill the Promise with an exception_wrapper, e.g.
    auto ew = folly::try_and_catch([]{ ... });
    if (ew) {
      p.setException(std::move(ew));
    }
    */
  void setException(folly::exception_wrapper);

  /**
   * Blocks task execution until given promise is fulfilled.
   *
   * Calls function passing in a Promise<T>, which has to be fulfilled.
   *
   * @return data which was used to fulfill the promise.
   */
  template <class F>
  static value_type await_async(F&& func);

#if !defined(_MSC_VER)
  template <class F>
  FOLLY_ERASE static value_type await(F&& func) {
    return await_sync(static_cast<F&&>(func));
  }
#endif

 private:
  Promise(folly::Try<T>& value, BatonT& baton);
  folly::Try<T>* value_;
  BatonT* baton_;

  void throwIfFulfilled() const;
};
} // namespace fibers
} // namespace folly

#include <folly/fibers/Promise-inl.h>
