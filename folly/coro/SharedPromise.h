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

#include <cstddef>
#include <type_traits>
#include <utility>

#include <folly/Likely.h>
#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/coro/Promise.h>
#include <folly/futures/Promise.h>
#include <folly/small_vector.h>

#if FOLLY_HAS_COROUTINES

namespace folly::coro {

/**
 * SharedPromise is a simple wrapper around folly::coro::Promise and
 * folly::coro::Future that allows for fetching cancellable and awaitable
 * futures from a single promise.
 *
 * It has the same behavior as folly::SharedPromise<>.  This includes the
 * difference in behavior of folly::SharedPromise and folly::Promise with
 * regards to invalid promise exceptions -- when SharedPromise<> is
 * moved from, calling setValue(), setTry(), or setException() don't result in
 * a PromiseInvalid exception.
 */
template <typename T>
class SharedPromise {
  using TryType = Try<lift_unit_t<T>>;

 public:
  /**
   * Constructors have behavior identical to folly::SharedPromise.
   */
  SharedPromise() = default;
  SharedPromise(SharedPromise&&) noexcept;
  SharedPromise& operator=(SharedPromise&&) noexcept;
  SharedPromise(const SharedPromise&) = delete;
  SharedPromise& operator=(const SharedPromise&) = delete;

  /**
   * Returns a future that is fulfilled when the user sets a value on the
   * promise.  Because this is a coro::Future, it supports cancellation.
   */
  folly::coro::Future<T> getFuture() const;

  /**
   * Returns the number of futures associated with the SharedPromise.
   */
  std::size_t size() const;

  /**
   * Returns true if the promise has either a value or an exception set.
   */
  bool isFulfilled() const;

  /*
   * Returns either an optional holding the result or an empty optional
   * depending on whether or not (respectively) the promise has been
   * fulfilled (i.e., `isFulfilled() == true`).
   */
  std::optional<TryType> poll() const;

  /**
   * Sets an exception in the promise.
   */
  void setException(folly::exception_wrapper&&);

  /**
   * Sets a value in the promise.
   */
  template <typename U = T>
  void setValue(U&&);
  template <typename U = T>
    requires std::is_void_v<U>
  void setValue();

  /**
   * Sets a folly::Try object in the promise.
   */
  void setTry(TryType&&);

 private:
  struct State {
    TryType result;
    folly::small_vector<folly::coro::Promise<T>> promises;
  };

  static bool isFulfilled(const State&);
  static void setTry(State&, TryType&&);

  mutable folly::Synchronized<State> state_;
};

template <typename T>
SharedPromise<T>::SharedPromise(SharedPromise&& other) noexcept
    : state_{std::exchange(*other.state_.wlock(), {})} {}

template <typename T>
SharedPromise<T>& SharedPromise<T>::operator=(SharedPromise&& other) noexcept {
  if (this != &other) [[likely]] {
    synchronized(
        [](auto self, auto other) { *self = std::exchange(*other, {}); },
        wlock(state_),
        wlock(other.state_));
  }
  return *this;
}

template <typename T>
folly::coro::Future<T> SharedPromise<T>::getFuture() const {
  return state_.withWLock([&](auto& state) {
    // if the promise already has a value, then we just return a ready future
    if (isFulfilled(state)) {
      if constexpr (std::is_void_v<T>) {
        return state.result.hasValue()
            ? folly::coro::makeFuture()
            : folly::coro::makeFuture<void>(
                  folly::copy(state.result.exception()));
      } else {
        return state.result.hasValue()
            ? folly::coro::makeFuture<T>(folly::copy(state.result.value()))
            : folly::coro::makeFuture<T>(folly::copy(state.result.exception()));
      }
    }

    auto [promise, future] = folly::coro::makePromiseContract<T>();
    state.promises.push_back(std::move(promise));
    return std::move(future);
  });
}

template <typename T>
std::size_t SharedPromise<T>::size() const {
  return state_.withRLock([](auto& state) { return state.promises.size(); });
}

template <typename T>
bool SharedPromise<T>::isFulfilled() const {
  return state_.withRLock([](auto& state) { return isFulfilled(state); });
}

template <typename T>
auto SharedPromise<T>::poll() const -> std::optional<TryType> {
  return state_.withRLock([](const auto& state) {
    return isFulfilled(state) ? std::make_optional(state.result) : std::nullopt;
  });
}

template <typename T>
void SharedPromise<T>::setException(folly::exception_wrapper&& exception) {
  state_.withWLock([&](auto& state) {
    setTry(state, TryType{std::move(exception)});
  });
}

template <typename T>
template <typename U>
void SharedPromise<T>::setValue(U&& input) {
  state_.withWLock([&](auto& state) {
    setTry(state, TryType{std::in_place, std::forward<U>(input)});
  });
}

template <typename T>
template <typename U>
  requires std::is_void_v<U>
void SharedPromise<T>::setValue() {
  setTry(TryType{unit});
}

template <typename T>
void SharedPromise<T>::setTry(TryType&& result) {
  state_.withWLock([&](auto& state) { setTry(state, std::move(result)); });
}

template <typename T>
bool SharedPromise<T>::isFulfilled(const SharedPromise<T>::State& state) {
  return state.result.hasException() || state.result.hasValue();
}

template <typename T>
void SharedPromise<T>::setTry(
    SharedPromise<T>::State& state, TryType&& result) {
  if (isFulfilled(state)) {
    throw_exception<PromiseAlreadySatisfied>();
  }

  auto promises = std::exchange(state.promises, {});
  for (auto& promise : promises) {
    promise.setResult(folly::copy(result));
  }

  state.result = std::move(result);
}

} // namespace folly::coro

#endif
