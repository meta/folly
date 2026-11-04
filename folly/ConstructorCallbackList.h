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
#include <array>
#include <atomic>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <fmt/format.h>
#include <folly/detail/StaticSingletonManager.h>

#include <folly/Function.h>
#include <folly/SharedMutex.h>

namespace folly {

// A mixin to register and issue callbacks every time a class constructor is
// invoked
//
// For example:
// #include <folly/ConstructorCallbackList.h>
//
// class Foo {
//    ...
//   private:
//    ...
//    // add this member last to minimize partially constructed errors
//    ConstructorCallbackList<Foo> constructorCB_{this};
// }
//
// int main() {
//   auto cb = [](Foo * f) {
//    std::cout << "New Foo" << f << std::endl;
//   };
//   ConstructorCallbackList<Foo>::addCallback(cb);
//   Foo f{}; // will call callback, print to stdout
// }
//
// This code is designed to be light weight so as to mixin to many
// places with low overhead.
//
// NOTE: The callback is triggered with a *partially* constructed object.
// This implies that that callback code can only access members that are
// constructed *before* the ConstructorCallbackList object.  Also, at the time
// of the callback, none of the Foo() constructor code will have run.
// Per the example above,
// the best practice is to place the ConstructorCallbackList declaration last
// in the parent class.  This will minimize the amount of uninitialized
// data in the Foo instance, but will not eliminate it unless it has a trivial
// constructor.
//
// Implementation/Overhead Notes:
//
// By design, adding ConstructorCallbackList to an object should be very
// light weight.  From a memory context, this adds 1 byte of memory to the
// parent class. From a CPU/performance perspective, the constructor does a load
// of an atomic int and the cost of the actual callbacks themselves.  So if this
// is put in place and only used infrequently, e.g., during debugging,
// this cost should be quite small.
//
// A compile-time static array is used intentionally over a dynamic one for
// two reasons: (1) a dynamic array seems to require a proper lock in
// the constructor which would exceed our perf target, and (2) having a
// finite array provides some sanity checking on the number of callbacks
// that can be registered.

template <class T, std::size_t MaxCallbacks = 4>
class ConstructorCallbackList {
 public:
  static constexpr std::size_t kMaxCallbacks = MaxCallbacks;

  using This = ConstructorCallbackList<T, MaxCallbacks>;
  using Callback = folly::Function<void(T*)>;
  using CallbackArray = std::array<typename This::Callback, MaxCallbacks>;

  explicit ConstructorCallbackList(T* t) {
    // This code depends on the C++ standard where values that are
    // initialized to zero ("Zero Initiation") are initialized before any more
    // complex static pre-main() dynamic initialization - see
    // https://en.cppreference.com/w/cpp/language/initialization) for
    // more details.
    //
    // This assumption prevents a subtle initialization race condition
    // where something could call this code pre-main() before
    // numCallbacks_ was set to zero, and thus prevents issuing
    // callbacks on garbage data.

    auto nCBs = This::global().numCallbacks_.load(std::memory_order_acquire);

    // fire callbacks to inform listeners about the new constructor
    /****
     * We don't need the full lock here, just the atomic int to tell us
     * how far into the array to go/how many callbacks are registered
     *
     * NOTE that nCBs > 0 will always imply that callbacks_ is non-nullptr
     */
    for (size_t i = 0; i < nCBs; i++) {
      (This::global().callbacks_)[i](t);
    }
  }

  /**
   * Add a callback to the static class that will fire every time
   * someone creates a new one.
   *
   * Implement this as a static array of callbacks rather than a dynamic
   * vector to avoid nasty race conditions on resize, startup and shutdown.
   *
   * Implement this with functions rather than an observer pattern classes
   * to avoid race conditions on shutdown
   *
   * Intentionally don't implement removeConstructorCallbackList to simplify
   * implementation (e.g., just the counter is atomic rather than the whole
   * array) and thus reduce computational cost.
   *
   * @throw std::length_error() if this callback would exceed our max
   */
  static void addCallback(Callback cb) {
    // Ensure that a single callback is added at a time
    std::lock_guard g(This::global().mutex_);
    auto idx = This::global().numCallbacks_.load(std::memory_order_acquire);

    if (idx >= (This::global().callbacks_).size()) {
      throw std::length_error(
          fmt::format("Too many callbacks - max {}", MaxCallbacks));
    }
    (This::global().callbacks_)[idx] = std::move(cb);
    // Only increment numCallbacks_ after fully initializing the array
    // entry. This step makes the new array entry visible to other threads.
    This::global().numCallbacks_.store(idx + 1, std::memory_order_release);
  }

 private:
  // use createGlobal to avoid races on shutdown
  struct GlobalStorage {
    mutable folly::SharedMutex mutex_;
    This::CallbackArray callbacks_{};
    std::atomic<size_t> numCallbacks_{0};
  };
  static auto& global() {
    return folly::detail::createGlobal<GlobalStorage, void>();
  }
};
} // namespace folly
