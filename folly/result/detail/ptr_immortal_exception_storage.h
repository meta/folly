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

#include <exception>

#include <folly/CppAttributes.h>
#include <folly/Portability.h> // FOLLY_HAS_RESULT
#include <folly/Traits.h>
#include <folly/lang/SafeAssert.h>
#include <folly/result/rich_exception_ptr.h>

#if FOLLY_HAS_RESULT

namespace folly::detail {

template <const auto* P>
class ptr_immortal_exception_storage : immortal_exception_storage {
 private:
  // Map `const immortal_rich_error_storage<B>*` -> `B`
  using UserBase = typename std::remove_const_t<std::remove_pointer_t<
      decltype(P)>>::folly_private_immortal_rich_error_base;
  using LeafRichError = ::folly::rich_error<UserBase>;

  LeafRichError* immutable_leaf_rich_error_singleton() const {
    // Union trick: the empty destructor ensures `value` is never destroyed.
    // NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
    union Storage {
      LeafRichError value;
      Storage() : value{immortal_rich_error_private_t{}, *P} {}
      ~Storage() {}
    };
    // Meyer singleton is thread-safe past C++11. Leak it to avoid SDOF.
    //
    // Yes, this will make an instance per DSO. That's expected and OK.
    // NOLINTNEXTLINE(facebook-hte-InlinedStaticLocalVariableWarning)
    static Storage storage;
    return &storage.value;
  }

  // In contrast to the immutable singleton, `rich_exception_ptr::operator==`
  // must be able to peek at the singleton **without** creating it -- doing so
  // would waste time & memory in programs trying to efficiently use immortals.
  // So, we can't use leaky Meyer here.
  class mutable_std_exception_leaf_rich_error_singleton {
   private:
    // Aligned byte storage for `std::exception_ptr` instead of an actual
    // `std::exception_ptr` (that would make us non-constant-initializable).
    //
    // Constructed via `ensure_created()` and leaked / immortal thereafter.
    alignas(std::exception_ptr) mutable char eptr_buf_[sizeof(
        std::exception_ptr)]{};
    // Initially null. After `ensure_created()`, points to `eptr_buf_`.
    mutable std::atomic<std::exception_ptr*> eptr_{nullptr};
    mutable std::mutex ensure_created_mutex_{};

    // Cold path of `get()`
    std::exception_ptr* ensure_created() const {
      std::lock_guard lock(ensure_created_mutex_);
      // Relaxed load ok -- the mutex synchronizes us with competing creators.
      auto* eptr = eptr_.load(std::memory_order_relaxed);
      if (FOLLY_LIKELY(!eptr)) {
        eptr = new (eptr_buf_) std::exception_ptr{make_exception_ptr_with(
            std::in_place_type<LeafRichError>,
            immortal_rich_error_private_t{},
            *P)};
        // Synchronizes with the load-acquires in `get()` and
        // `acquire_ptr_if_already_created()`.
        eptr_.store(eptr, std::memory_order_release);
      }
      FOLLY_SAFE_DCHECK(eptr != nullptr);
      return eptr;
    }

   public:
    std::exception_ptr& get() const {
      auto* eptr = eptr_.load(std::memory_order_acquire);
      if (FOLLY_UNLIKELY(!eptr)) {
        eptr = ensure_created();
      }
      FOLLY_SAFE_DCHECK(eptr != nullptr);
      return *eptr;
    }

    // Used by `operator==` to avoid unnecessarily creating the eptr
    std::exception_ptr* acquire_ptr_if_already_created() const {
      return eptr_.load(std::memory_order_acquire);
    }
  };
  // Other statics may compare an immortal error in their initializers, or first
  // convert it to `std::exception_ptr` in their destructors. The holder must be
  // ready for the comparison and keep its creation mutex usable through
  // shutdown. Without `no_destroy`, the holder needs a trivial destructor:
  // otherwise, a later static destructor could try to create the exception by
  // locking a mutex that has already been destroyed.
  [[FOLLY_ATTR_CLANG_NO_DESTROY]] constinit inline static const mutable_std_exception_leaf_rich_error_singleton
      mutable_eptr_singleton_{};
  static_assert(
      FOLLY_HAS_CPP_ATTRIBUTE(clang::no_destroy) ||
      std::is_trivially_destructible_v<
          mutable_std_exception_leaf_rich_error_singleton>);

  template <typename Ex>
  Ex* get_mutable_exception_impl() const {
    // The `LeafRichError` cast is safe, see `ensure_created()` above.
    return static_cast<Ex*>(static_cast<LeafRichError*>(
        exception_ptr_get_object(mutable_eptr_singleton_.get())));
  }

 protected:
  template <typename, typename, auto...>
  friend class folly::immortal_rich_error_t;
  consteval ptr_immortal_exception_storage()
      : immortal_exception_storage{
            P, &typeid(UserBase), &typeid(LeafRichError)} {}

  [[noreturn]] void throw_exception() const override {
    throw *immutable_leaf_rich_error_singleton();
  }

  // Since `std::exception` isn't `constexpr` before C++26, these two have to
  // be backed by an immutable leaky singleton.
  const rich_error_base* as_immutable_leaf_rich_error() const override {
    return immutable_leaf_rich_error_singleton();
  }
  const std::exception* as_immutable_std_exception() const override {
    return immutable_leaf_rich_error_singleton();
  }

  // `to_exception_ptr()` and `folly::get_mutable_exception()` give mutable
  // access to an exception object.  A mutable singleton backs both.
  rich_error_base* as_mutable_leaf_rich_error() const override {
    return get_mutable_exception_impl<LeafRichError>();
  }
  std::exception* as_mutable_std_exception() const override {
    return get_mutable_exception_impl<std::exception>();
  }
  std::exception_ptr* acquire_mutable_singleton_ptr_if_already_created()
      const override {
    return mutable_eptr_singleton_.acquire_ptr_if_already_created();
  }
  std::exception_ptr to_exception_ptr() const override {
    // Future: This can avoid atomic refcount ops on __GLIBCXX__ and
    // _LIBCPP_VERSION build flavors, as follows.
    //   - Add new detail helpers to `lang/Exception.cpp`, one to make an
    //     immortal `std::exception_ptr` with a HUGE refcount, and the other to
    //     cheaply copy it without bumping the refcount.  See `referenceCount`
    //     in `make_exception_ptr_with_()`.
    //   - For example, the initial refcount can be 2^63, and the copy
    //     method only adds 2^63 when the count drops below 2^60.
    //   - Use the helpers to construct / copy the eptr here.
    return mutable_eptr_singleton_.get();
  }
};

} // namespace folly::detail

#endif // FOLLY_HAS_RESULT
