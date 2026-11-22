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
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/Traits.h>

// On MSVC an incorrect <version> header get's picked up
#if !defined(_MSC_VER) && __has_include(<version>)
#include <version>
#endif

namespace {
// This struct is different in every translation unit.  We use template
// instantiations to define inline freestanding methods.  Since the
// methods are inline it is fine to define them in multiple translation
// units, but the instantiation itself would be an ODR violation if it is
// present in the program more than once.  By tagging the instantiations
// with this struct, we avoid ODR problems for the instantiation while
// allowing the resulting methods to be inline-able.  If you think that
// seems hacky keep reading...
struct FollyMemoryDetailTranslationUnitTag {};
} // namespace
namespace folly {
namespace detail {
template <typename T>
void unsafeStringSetLargerSize(std::basic_string<T>& s, std::size_t n);
template <typename T>
void unsafeVectorSetLargerSize(std::vector<T>& v, std::size_t n);
} // namespace detail

/*
 * This file provides helper functions resizeWithoutInitialization()
 * that can resize containers without constructing or initializing new
 * elements.  Support is provided for std::basic_string, std::vector, and
 * any container that exposes resize_without_initialization().
 *
 * IMPORTANT: These functions can be unsafe if used improperly.  If you
 * don't write to an element with index >= oldSize and < newSize, reading
 * the element can expose arbitrary memory contents to the world, including
 * the contents of old strings.  If you're lucky you'll get a segfault,
 * because the kernel is only required to fault in new pages on write
 * access.  MSAN should be able to catch problems in the common case that
 * the string or vector wasn't previously shrunk.
 *
 * Pay extra attention to your failure paths.  For example, if you try
 * to read directly into a caller-provided string, make sure to clear
 * the string when you get an I/O error.
 *
 * You should only use this if you have profiling data from production
 * that shows that this is not a premature optimization.  This code is
 * designed for retroactively optimizing code where touching every element
 * twice (or touching never-used elements once) shows up in profiling,
 * and where restructuring the code to use fixed-length arrays or IOBuf-s
 * would be difficult.
 *
 * NOTE: Just because .resize() shows up in your profile (probably
 * via one of the intrinsic memset implementations) doesn't mean that
 * these functions will make your program faster.  A lot of the cost
 * of memset comes from cache misses, so avoiding the memset can mean
 * that the cache miss cost just gets pushed to the following code.
 * resizeWithoutInitialization can be a win when the contents are bigger
 * than a cache level, because the second access isn't free in that case.
 * It can be a win when the memory is already cached, so touching it
 * doesn't help later code.  It can also be a win if the final length
 * of the string or vector isn't actually known, so the suffix will be
 * chopped off with a second call to .resize().
 */

namespace detail {

/**
 * Like calling s.resize(n), but when growing the string does not
 * initialize new elements.  It is undefined behavior to read from
 * any element added to the string by this method unless it has been
 * written to by an operation that follows this call.
 *
 * Use the FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT(T) macro to
 * declare (and inline define) the internals required to call
 * resizeWithoutInitialization for a std::basic_string<T>.
 * See detailed description of a similar macro for std::vector<T> below.
 *
 * IMPORTANT: Read the warning at the top of this header file.
 */
template <typename T>
  requires std::is_trivially_destructible_v<T>
inline void resizeWithoutInitializationImpl(
    std::basic_string<T>& s, std::size_t n) {
  if (n <= s.size()) {
    s.resize(n);
  } else {
    // careful not to call reserve unless necessary, as it causes
    // shrink_to_fit on many platforms
    if (n > s.capacity()) {
      s.reserve(n);
    }
    unsafeStringSetLargerSize(s, n);
  }
}

/**
 * Like calling v.resize(n), but when growing the vector does not construct
 * or initialize new elements.  It is undefined behavior to read from any
 * element added to the vector by this method unless it has been written
 * to by an operation that follows this call.
 *
 * Use the FOLLY_DECLARE_VECTOR_RESIZE_WITHOUT_INIT(T) macro to
 * declare (and inline define) the internals required to call
 * resizeWithoutInitialization for a std::vector<T>.  This must
 * be done exactly once in each translation unit that wants to call
 * resizeWithoutInitialization(std::vector<T>&,size_t).  char and unsigned
 * char are provided by default.  If you don't do this you will get linker
 * errors about folly::detail::unsafeVectorSetLargerSize.  Requiring that
 * T be trivially_destructible is only an approximation of the property
 * required of T.  In fact what is required is that any random sequence of
 * bytes may be safely reinterpreted as a T and passed to T's destructor.
 *
 * std::vector<bool> has specialized internals and is not supported.
 *
 * IMPORTANT: Read the warning at the top of this header file.
 */
template <typename T>
  requires(std::is_trivially_destructible_v<T> && !std::is_same_v<T, bool>)
void resizeWithoutInitializationImpl(std::vector<T>& v, std::size_t n) {
  if (n <= v.size()) {
    v.resize(n);
  } else {
    if (n > v.capacity()) {
      v.reserve(n);
    }
    unsafeVectorSetLargerSize(v, n);
  }
}

template <typename C, typename... A>
using detect_resize_without_initialization_member =
    decltype(std::declval<C&>().resize_without_initialization(
        std::declval<A>()...));

template <typename C>
using resize_without_initialization_value_type =
    typename std::remove_cvref_t<C>::value_type;

template <typename C>
inline constexpr bool has_resize_without_initialization_member_v =
    is_detected_v<
        detect_resize_without_initialization_member,
        C,
        std::size_t> &&
    std::is_trivially_destructible_v<
        detected_t<resize_without_initialization_value_type, C>>;

struct resize_without_initialization_fn {
  template <typename C>
    requires has_resize_without_initialization_member_v<C>
  void operator()(C& c, std::size_t n) const {
    c.resize_without_initialization(n);
  }

  template <typename C>
  auto operator()(C& c, std::size_t n) const
      -> decltype(resizeWithoutInitializationImpl(c, n)) {
    resizeWithoutInitializationImpl(c, n);
  }
};

} // namespace detail

inline constexpr detail::resize_without_initialization_fn
    resizeWithoutInitialization{};

namespace detail {

// This machinery bridges template expansion and macro expansion
#define FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT_IMPL(TYPE)                    \
  namespace folly {                                                            \
  namespace detail {                                                           \
  void unsafeStringSetLargerSizeImpl(std::basic_string<TYPE>& s, std::size_t); \
  template <>                                                                  \
  inline void unsafeStringSetLargerSize<TYPE>(                                 \
      std::basic_string<TYPE> & s, std::size_t n) {                            \
    unsafeStringSetLargerSizeImpl(s, n);                                       \
  }                                                                            \
  }                                                                            \
  }

#if defined(_LIBCPP_STRING)
// libc++

template <typename Tag, typename T, typename A, A Ptr__set_size>
struct MakeUnsafeStringSetLargerSize {
  friend void unsafeStringSetLargerSizeImpl(
      std::basic_string<T>& s, std::size_t n) {
    // s.__set_size(n);
    (s.*Ptr__set_size)(n);
    (&s[0])[n] = '\0';
  }
};

#define FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT(TYPE)            \
  template void std::basic_string<TYPE>::__set_size(std::size_t); \
  template struct folly::detail::MakeUnsafeStringSetLargerSize<   \
      FollyMemoryDetailTranslationUnitTag,                        \
      TYPE,                                                       \
      void (std::basic_string<TYPE>::*)(std::size_t),             \
      &std::basic_string<TYPE>::__set_size>;                      \
  FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT_IMPL(TYPE)

#elif defined(_GLIBCXX_STRING) && _GLIBCXX_USE_CXX11_ABI
// libstdc++ new implementation with SSO

template <typename Tag, typename T, typename A, A Ptr_M_set_length>
struct MakeUnsafeStringSetLargerSize {
  friend void unsafeStringSetLargerSizeImpl(
      std::basic_string<T>& s, std::size_t n) {
    // s._M_set_length(n);
    (s.*Ptr_M_set_length)(n);
  }
};

#define FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT(TYPE)               \
  template void std::basic_string<TYPE>::_M_set_length(std::size_t); \
  template struct folly::detail::MakeUnsafeStringSetLargerSize<      \
      FollyMemoryDetailTranslationUnitTag,                           \
      TYPE,                                                          \
      void (std::basic_string<TYPE>::*)(std::size_t),                \
      &std::basic_string<TYPE>::_M_set_length>;                      \
  FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT_IMPL(TYPE)

#elif defined(_GLIBCXX_STRING)
// libstdc++ old implementation

template <
    typename Tag,
    typename T,
    typename A,
    A Ptr_M_rep,
    typename B,
    B Ptr_M_set_length_and_sharable>
struct MakeUnsafeStringSetLargerSize {
  friend void unsafeStringSetLargerSizeImpl(
      std::basic_string<T>& s, std::size_t n) {
    // s._M_rep()->_M_set_length_and_sharable(n);
    auto rep = (s.*Ptr_M_rep)();
    (rep->*Ptr_M_set_length_and_sharable)(n);
  }
};

#define FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT(TYPE)                      \
  template std::basic_string<TYPE>::_Rep* std::basic_string<TYPE>::_M_rep() \
      const;                                                                \
  template void std::basic_string<TYPE>::_Rep::_M_set_length_and_sharable(  \
      std::size_t);                                                         \
  template struct folly::detail::MakeUnsafeStringSetLargerSize<             \
      FollyMemoryDetailTranslationUnitTag,                                  \
      TYPE,                                                                 \
      std::basic_string<TYPE>::_Rep* (std::basic_string<TYPE>::*)() const,  \
      &std::basic_string<TYPE>::_M_rep,                                     \
      void (std::basic_string<TYPE>::_Rep::*)(std::size_t),                 \
      &std::basic_string<TYPE>::_Rep::_M_set_length_and_sharable>;          \
  FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT_IMPL(TYPE)

#elif defined(_MSC_VER)

template <typename Tag, typename T, typename A, A Ptr_Eos>
struct MakeUnsafeStringSetLargerSize {
  friend void unsafeStringSetLargerSizeImpl(
      std::basic_string<T>& s, std::size_t n) {
    (s.*Ptr_Eos)(n);
  }
};

#if _MSC_VER < 1939
#define FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT(TYPE)          \
  template void std::basic_string<TYPE>::_Eos(std::size_t);     \
  template struct folly::detail::MakeUnsafeStringSetLargerSize< \
      FollyMemoryDetailTranslationUnitTag,                      \
      TYPE,                                                     \
      void (std::basic_string<TYPE>::*)(std::size_t),           \
      &std::basic_string<TYPE>::_Eos>;                          \
  FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT_IMPL(TYPE)
#else
#define FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT(TYPE)          \
  template struct folly::detail::MakeUnsafeStringSetLargerSize< \
      FollyMemoryDetailTranslationUnitTag,                      \
      TYPE,                                                     \
      void (std::basic_string<TYPE>::*)(std::size_t),           \
      &std::basic_string<TYPE>::_Eos>;                          \
  FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT_IMPL(TYPE)
#endif // _MSC_VER < 1939
#else
#warning \
    "No implementation for resizeWithoutInitialization of std::basic_string"
#endif

} // namespace detail
} // namespace folly

#if defined(FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT)
FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT(char)
FOLLY_DECLARE_STRING_RESIZE_WITHOUT_INIT(wchar_t)
#endif

namespace folly {
namespace detail {

// This machinery bridges template expansion and macro expansion
#define FOLLY_DECLARE_VECTOR_RESIZE_WITHOUT_INIT_IMPL(TYPE)              \
  namespace folly {                                                      \
  namespace detail {                                                     \
  void unsafeVectorSetLargerSizeImpl(std::vector<TYPE>& v, std::size_t); \
  template <>                                                            \
  inline void unsafeVectorSetLargerSize<TYPE>(                           \
      std::vector<TYPE> & v, std::size_t n) {                            \
    unsafeVectorSetLargerSizeImpl(v, n);                                 \
  }                                                                      \
  }                                                                      \
  }

#if defined(_LIBCPP_VECTOR) && (_LIBCPP_VERSION < 200000)
// libc++

template <typename T, typename Alloc = std::allocator<T>>
struct std_vector_layout {
  static_assert(!std::is_same<T, bool>::value, "bad instance");
  using allocator_type = Alloc;
  using pointer = typename std::allocator_traits<allocator_type>::pointer;

  pointer __begin_;
  pointer __end_;
#ifdef _LIBCPP_COMPRESSED_PAIR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wgnu-anonymous-struct"
  _LIBCPP_COMPRESSED_PAIR(pointer, __cap_ = nullptr, allocator_type, __alloc_);
#pragma GCC diagnostic pop
#else
  std::__compressed_pair<pointer, allocator_type> __end_cap_;
#endif
};

// Precompute the offset in a variable-template initializer. The clang-21
// parser crashes (SIGSEGV) when offsetof is applied to a template-dependent
// type inside a function-template body; std_vector_layout<T> is still
// dependent here, but evaluating offsetof in this context sidesteps the crash.
template <typename T>
inline constexpr std::size_t kVectorEndOffset =
    offsetof(std_vector_layout<T>, __end_);

template <typename T>
void unsafeVectorSetLargerSize(std::vector<T>& v, std::size_t n) {
  using real = std::vector<T>;
  using fake = std_vector_layout<T>;
  using pointer = typename fake::pointer;
  static_assert(sizeof(fake) == sizeof(real), "mismatch");
  static_assert(alignof(fake) == alignof(real), "mismatch");

  auto const l = reinterpret_cast<unsigned char*>(&v);

  auto const s = v.size();

  auto& e = *reinterpret_cast<pointer*>(l + kVectorEndOffset<T>);
  e += (n - s);

  // libc++ contiguous containers use special annotation functions that help
  // the address sanitizer to detect improper memory accesses. When ASAN is
  // enabled we need to call the appropriate annotation functions in order to
  // stop ASAN from reporting false positives. When ASAN is disabled, the
  // annotation function is a no-op.
#if defined(_LIBCPP_HAS_ASAN)
#define FOLLY_ASAN_ANNOTATE_CONTIGUOUS_CONTAINER _LIBCPP_HAS_ASAN
#elif defined(_LIBCPP_HAS_NO_ASAN)
#define FOLLY_ASAN_ANNOTATE_CONTIGUOUS_CONTAINER 0
#elif __has_feature(address_sanitizer)
#define FOLLY_ASAN_ANNOTATE_CONTIGUOUS_CONTAINER 1
#else
#define FOLLY_ASAN_ANNOTATE_CONTIGUOUS_CONTAINER 0
#endif

#if FOLLY_ASAN_ANNOTATE_CONTIGUOUS_CONTAINER
  __sanitizer_annotate_contiguous_container(
      v.data(), v.data() + v.capacity(), v.data() + s, v.data() + n);
#endif
}

#define FOLLY_DECLARE_VECTOR_RESIZE_WITHOUT_INIT(TYPE)

#elif defined(_GLIBCXX_VECTOR)
// libstdc++

// std::vector<T>::_M_impl is protected, so a derived class may access it
// through any std::vector<T>.
template <typename T>
struct UnsafeVectorSetLargerSize : std::vector<T> {
  static void apply(std::vector<T>& v, std::size_t n) {
    // v._M_impl._M_finish += (n - v.size());
    (v.*&UnsafeVectorSetLargerSize::_M_impl)._M_finish += (n - v.size());
  }
};

template <typename T>
void unsafeVectorSetLargerSize(std::vector<T>& v, std::size_t n) {
  UnsafeVectorSetLargerSize<T>::apply(v, n);
}

#define FOLLY_DECLARE_VECTOR_RESIZE_WITHOUT_INIT(TYPE)

#elif defined(_MSC_VER)

template <
    typename Tag,
    typename T,
    typename A,
    A Ptr_Mypair,
    typename B,
    B Ptr_Myval2,
    typename C,
    C Ptr_Mylast>
struct MakeUnsafeVectorSetLargerSize : std::vector<T> {
  friend void unsafeVectorSetLargerSizeImpl(std::vector<T>& v, std::size_t n) {
    // v._Mypair._Myval2._Mylast += (n - v.size());
    ((v.*Ptr_Mypair).*Ptr_Myval2).*Ptr_Mylast += (n - v.size());
  }
};

#define FOLLY_DECLARE_VECTOR_RESIZE_WITHOUT_INIT(TYPE)                         \
  template struct folly::detail::MakeUnsafeVectorSetLargerSize<                \
      FollyMemoryDetailTranslationUnitTag,                                     \
      TYPE,                                                                    \
      decltype(&std::vector<TYPE>::_Mypair),                                   \
      &std::vector<TYPE>::_Mypair,                                             \
      decltype(&decltype(std::declval<std::vector<TYPE>>()._Mypair)::_Myval2), \
      &decltype(std::declval<std::vector<TYPE>>()._Mypair)::_Myval2,           \
      decltype(&decltype(std::declval<std::vector<TYPE>>()                     \
                             ._Mypair._Myval2)::_Mylast),                      \
      &decltype(std::declval<std::vector<TYPE>>()._Mypair._Myval2)::_Mylast>;  \
  FOLLY_DECLARE_VECTOR_RESIZE_WITHOUT_INIT_IMPL(TYPE)

#else
#warning "No implementation for resizeWithoutInitialization of std::vector"
#endif

} // namespace detail
} // namespace folly

#if defined(FOLLY_DECLARE_VECTOR_RESIZE_WITHOUT_INIT)
FOLLY_DECLARE_VECTOR_RESIZE_WITHOUT_INIT(char)
FOLLY_DECLARE_VECTOR_RESIZE_WITHOUT_INIT(unsigned char)
#endif
