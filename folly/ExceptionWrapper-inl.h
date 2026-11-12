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

#include <type_traits>

#include <folly/Portability.h>

namespace folly {

struct exception_wrapper::with_exception_from_fn_ {
  struct impl_var_ {
    template <typename>
    using apply = void;
  };
  struct impl_arg_ {
    template <typename F>
    using apply = function_arguments_element_t<0, F>;
  };
  struct impl_bye_;
  template <typename Sig, std::size_t NArgs = function_arguments_size_v<Sig>>
  using impl_ = conditional_t<
      function_is_variadic_v<Sig>,
      impl_var_,
      conditional_t<NArgs == 1, impl_arg_, impl_bye_>>;

  template <typename T>
  using member_ = typename member_pointer_traits<T>::member_type;

  template <typename Void, typename>
  struct arg_type_;
  template <class Sig>
  struct arg_type_<std::enable_if_t<std::is_function<Sig>::value>, Sig> {
    using type = typename impl_<Sig>::template apply<Sig>;
  };
  template <class Ptr>
  struct arg_type_<std::enable_if_t<std::is_pointer<Ptr>::value>, Ptr>
      : arg_type_<void, std::remove_pointer_t<Ptr>> {};
  template <class Obj>
  struct arg_type_<std::void_t<decltype(&Obj::operator())>, Obj>
      : arg_type_<void, member_<decltype(&Obj::operator())>> {};

  // void if Fn is a variadic callable; otherwise the first arg type
  template <typename, typename Fn>
  using apply = _t<arg_type_<void, Fn>>;
};

struct exception_wrapper::with_exception_from_ex_ {
  template <typename Ex, typename>
  using apply = Ex;
};

inline exception_wrapper::exception_wrapper(exception_wrapper&& that) noexcept
    : ptr_{detail::extract_exception_ptr(std::move(that.ptr_))} {}

inline exception_wrapper::exception_wrapper(
    std::exception_ptr const& ptr) noexcept
    : ptr_{ptr} {}

inline exception_wrapper::exception_wrapper(std::exception_ptr&& ptr) noexcept
    : ptr_{detail::extract_exception_ptr(std::move(ptr))} {}

template <
    class Ex,
    class Ex_,
    std::enable_if_t<
        std::conjunction_v<
            exception_wrapper::IsStdException<Ex_>,
            exception_wrapper::IsRegularExceptionType<Ex_>>,
        int>>
inline exception_wrapper::exception_wrapper(Ex&& ex)
    : ptr_{make_exception_ptr_with(std::in_place, std::forward<Ex>(ex))} {}

template <class Ex>
inline exception_wrapper::exception_wrapper(std::in_place_t, Ex&& ex)
  requires(IsRegularExceptionType<std::decay_t<Ex>>::value)
    : ptr_{make_exception_ptr_with(std::in_place, std::forward<Ex>(ex))} {}

template <class Ex, typename... As>
inline exception_wrapper::exception_wrapper(
    std::in_place_type_t<Ex>, As&&... as)
  requires(IsRegularExceptionType<Ex>::value)
    : ptr_{make_exception_ptr_with(
          std::in_place_type<Ex>, std::forward<As>(as)...)} {}

inline exception_wrapper& exception_wrapper::operator=(
    exception_wrapper&& that) noexcept {
  // assume relocatability on all platforms
  constexpr auto sz = sizeof(std::exception_ptr);
  std::exception_ptr tmp;
  std::memcpy(static_cast<void*>(&tmp), &ptr_, sz);
  std::memcpy(static_cast<void*>(&ptr_), &that.ptr_, sz);
  std::memset(static_cast<void*>(&that.ptr_), 0, sz);
  return *this;
}

inline void exception_wrapper::swap(exception_wrapper& that) noexcept {
  // assume relocatability on all platforms
  constexpr auto sz = sizeof(std::exception_ptr);
  aligned_storage_for_t<std::exception_ptr> storage;
  std::memcpy(&storage, &ptr_, sz);
  std::memcpy(static_cast<void*>(&ptr_), &that.ptr_, sz);
  std::memcpy(static_cast<void*>(&that.ptr_), &storage, sz);
}

inline exception_wrapper::operator bool() const noexcept {
  return !!ptr_;
}

inline bool exception_wrapper::operator!() const noexcept {
  return !ptr_;
}

inline void exception_wrapper::reset() {
  ptr_ = {};
}

inline bool exception_wrapper::has_exception_ptr() const noexcept {
  return !!ptr_;
}

inline std::exception* exception_wrapper::get_mutable_exception()
    const noexcept {
  return exception_ptr_get_object<std::exception>(ptr_);
}
inline std::exception const* exception_wrapper::get_exception() const noexcept {
  return exception_ptr_get_object<std::exception>(ptr_);
}

template <typename Ex>
inline Ex* exception_wrapper::get_mutable_exception() const noexcept {
  return exception_ptr_get_object_hint<Ex>(ptr_);
}

template <typename Ex>
inline Ex const* exception_wrapper::get_exception() const noexcept {
  return exception_ptr_get_object_hint<Ex>(ptr_);
}

inline std::exception_ptr exception_wrapper::to_exception_ptr()
    const& noexcept {
  return ptr_;
}

inline std::exception_ptr& exception_wrapper::exception_ptr() & noexcept {
  return ptr_;
}
inline std::exception_ptr const& exception_wrapper::exception_ptr()
    const& noexcept {
  return ptr_;
}
inline std::exception_ptr&& exception_wrapper::exception_ptr() && noexcept {
  return std::move(ptr_);
}
inline std::exception_ptr const&& exception_wrapper::exception_ptr()
    const&& noexcept {
  return std::move(ptr_);
}

inline std::type_info const* exception_wrapper::type() const noexcept {
  return exception_ptr_get_type(ptr_);
}

inline folly::fbstring exception_wrapper::what() const {
  if (auto e = get_exception()) {
    return class_name() + ": " + e->what();
  }
  return class_name();
}

inline folly::fbstring exception_wrapper::class_name() const {
  auto const* const ti = type();
  return !*this ? "" : !ti ? "<unknown>" : folly::demangle(*ti);
}

template <class Ex>
inline bool exception_wrapper::is_compatible_with() const noexcept {
  return exception_ptr_get_object<Ex>(ptr_);
}

[[noreturn]] inline void exception_wrapper::throw_exception() const {
  ptr_ ? std::rethrow_exception(ptr_) : onNoExceptionError(__func__);
}

template <class Ex>
[[noreturn]] inline void exception_wrapper::throw_with_nested(Ex&& ex) const {
  try {
    throw_exception();
  } catch (...) {
    std::throw_with_nested(std::forward<Ex>(ex));
  }
}

template <class This, class Fn>
inline bool exception_wrapper::with_exception_(This&, Fn fn_, tag_t<void>) {
  return void(fn_()), true;
}

template <class This, class Fn, typename Ex>
inline bool exception_wrapper::with_exception_(This& this_, Fn fn_, tag_t<Ex>) {
  if constexpr (std::is_const_v<Ex>) {
    auto ptr = this_.template get_exception<std::remove_cvref_t<Ex>>();
    return ptr && (void(fn_(static_cast<Ex&>(*ptr))), true);
  } else {
    auto ptr = this_.template get_mutable_exception<std::remove_cvref_t<Ex>>();
    return ptr && (void(fn_(static_cast<Ex&>(*ptr))), true);
  }
}

template <class Ex, class This, class Fn>
inline bool exception_wrapper::with_exception_(This& this_, Fn fn_) {
  using from_fn = with_exception_from_fn_;
  using from_ex = with_exception_from_ex_;
  using from = conditional_t<std::is_void<Ex>::value, from_fn, from_ex>;
  using type = typename from::template apply<Ex, Fn>;
  return with_exception_(this_, std::move(fn_), tag<type>);
}

template <class This, class... CatchFns>
inline void exception_wrapper::handle_(
    This& this_, char const* name, CatchFns&... fns) {
  if (!this_) {
    onNoExceptionError(name);
  }
  if (!(with_exception_<void>(this_, fns) || ...)) {
    this_.throw_exception();
  }
}

template <class Ex, class Fn>
inline bool exception_wrapper::with_exception(Fn fn) {
  return with_exception_<Ex>(*this, std::move(fn));
}
template <class Ex, class Fn>
inline bool exception_wrapper::with_exception(Fn fn) const {
  return with_exception_<Ex const>(*this, std::move(fn));
}

template <class... CatchFns>
inline void exception_wrapper::handle(CatchFns... fns) {
  handle_(*this, __func__, fns...);
}
template <class... CatchFns>
inline void exception_wrapper::handle(CatchFns... fns) const {
  handle_(*this, __func__, fns...);
}

} // namespace folly
