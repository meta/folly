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

/**
 * GCC compatible wrappers around clang attributes.
 */

#pragma once

#include <folly/Portability.h>

#ifndef __has_attribute
#define FOLLY_HAS_ATTRIBUTE(x) 0
#else
#define FOLLY_HAS_ATTRIBUTE(x) __has_attribute(x)
#endif

#ifndef __has_extension
#define FOLLY_HAS_EXTENSION(x) 0
#else
#define FOLLY_HAS_EXTENSION(x) __has_extension(x)
#endif

/**
 * Nullable indicates that a return value or a parameter may be a `nullptr`,
 * e.g.
 *
 * int* FOLLY_NULLABLE foo(int* a, int* FOLLY_NULLABLE b) {
 *   if (*a > 0) {  // safe dereference
 *     return nullptr;
 *   }
 *   if (*b < 0) {  // unsafe dereference
 *     return *a;
 *   }
 *   if (b != nullptr && *b == 1) {  // safe checked dereference
 *     return new int(1);
 *   }
 *   return nullptr;
 * }
 *
 * Ignores Clang's -Wnullability-extension since it correctly handles the case
 * where the extension is not present.
 */
#if FOLLY_HAS_EXTENSION(nullability)
#define FOLLY_NULLABLE                                   \
  FOLLY_PUSH_WARNING                                     \
  FOLLY_CLANG_DISABLE_WARNING("-Wnullability-extension") \
  _Nullable FOLLY_POP_WARNING
#define FOLLY_NONNULL                                    \
  FOLLY_PUSH_WARNING                                     \
  FOLLY_CLANG_DISABLE_WARNING("-Wnullability-extension") \
  _Nonnull FOLLY_POP_WARNING
#else
#define FOLLY_NULLABLE
#define FOLLY_NONNULL
#endif

/**
 * "Cold" indicates to the compiler that a function is only expected to be
 * called from unlikely code paths. It can affect decisions made by the
 * optimizer both when processing the function body and when analyzing
 * call-sites.
 */
#if __has_cpp_attribute(gnu::cold)
#define FOLLY_ATTR_GNU_COLD gnu::cold
#else
#define FOLLY_ATTR_GNU_COLD
#endif

/// FOLLY_ATTR_GNU_NODEBUG
///
/// Excludes the annotated function from debug info. Useful for trivial
/// functions with many instantiations, such as generated per-field accessors.
#if __has_cpp_attribute(gnu::nodebug)
#define FOLLY_ATTR_GNU_NODEBUG gnu::nodebug
#else
#define FOLLY_ATTR_GNU_NODEBUG
#endif

/// FOLLY_ATTR_CLANG_CODE_ALIGN
///
/// Aligns the start of the loop that follows to an `n`-byte boundary, e.g. so
/// a tiny hot loop never straddles a 64-byte fetch line. Expands to nothing
/// on compilers without `[[clang::code_align]]`.
#if __has_cpp_attribute(clang::code_align)
#define FOLLY_ATTR_CLANG_CODE_ALIGN(n) clang::code_align(n)
#else
#define FOLLY_ATTR_CLANG_CODE_ALIGN(n)
#endif

/// FOLLY_ATTR_CLANG_OPTNONE
///
/// Disables optimization for the annotated function, e.g. to work around a
/// compiler miscompile. Expands to nothing on compilers without
/// `[[clang::optnone]]`.
#if __has_cpp_attribute(clang::optnone)
#define FOLLY_ATTR_CLANG_OPTNONE clang::optnone
#else
#define FOLLY_ATTR_CLANG_OPTNONE
#endif

/// FOLLY_ATTR_MAYBE_UNUSED_IF_NDEBUG
///
/// When defined(NDEBUG), expands to maybe_unused; otherwise, expands to empty.
/// Useful for marking variables that are used, in the sense checked for by the
/// attribute maybe_unused, only in debug builds.
#if defined(NDEBUG)
#define FOLLY_ATTR_MAYBE_UNUSED_IF_NDEBUG maybe_unused
#else
#define FOLLY_ATTR_MAYBE_UNUSED_IF_NDEBUG
#endif

/**
 *  no_unique_address indicates that a member variable can be optimized to
 * occupy no space, rather than the minimum 1-byte used by default.
 *
 *  class Empty {};
 *
 *  class NonEmpty1 {
 *    [[FOLLY_ATTR_NO_UNIQUE_ADDRESS]] Empty e;
 *    int f;
 *  };
 *
 *  class NonEmpty2 {
 *    Empty e;
 *    int f;
 *  };
 *
 *  sizeof(NonEmpty1); // may be == sizeof(int)
 *  sizeof(NonEmpty2); // must be > sizeof(int)
 */
#if __has_cpp_attribute(no_unique_address)
#define FOLLY_ATTR_NO_UNIQUE_ADDRESS no_unique_address
#elif __has_cpp_attribute(msvc::no_unique_address)
#define FOLLY_ATTR_NO_UNIQUE_ADDRESS msvc::no_unique_address
#else
#define FOLLY_ATTR_NO_UNIQUE_ADDRESS
#endif

#if __has_cpp_attribute(clang::no_destroy)
#define FOLLY_ATTR_CLANG_NO_DESTROY clang::no_destroy
#else
#define FOLLY_ATTR_CLANG_NO_DESTROY
#endif

#if __has_cpp_attribute(clang::uninitialized)
#define FOLLY_ATTR_CLANG_UNINITIALIZED clang::uninitialized
#else
#define FOLLY_ATTR_CLANG_UNINITIALIZED
#endif

/**
 * Accesses to objects with types with this attribute are not subjected to
 * type-based alias analysis, but are instead assumed to be able to alias any
 * other type of objects, just like the char type.
 */
#if __has_cpp_attribute(gnu::may_alias)
#define FOLLY_ATTR_GNU_MAY_ALIAS gnu::may_alias
#else
#define FOLLY_ATTR_GNU_MAY_ALIAS
#endif

#if __has_cpp_attribute(gnu::pure)
#define FOLLY_ATTR_GNU_PURE gnu::pure
#else
#define FOLLY_ATTR_GNU_PURE
#endif

#if __has_cpp_attribute(clang::preserve_most)
#define FOLLY_ATTR_CLANG_PRESERVE_MOST clang::preserve_most
#else
#define FOLLY_ATTR_CLANG_PRESERVE_MOST
#endif

#if __has_cpp_attribute(clang::preserve_all)
#define FOLLY_ATTR_CLANG_PRESERVE_ALL clang::preserve_all
#else
#define FOLLY_ATTR_CLANG_PRESERVE_ALL
#endif

/// FOLLY_ATTR_CLANG_PRESERVE_NONE
///
/// Applies the preserve-none calling convention, preserving no registers
/// across calls. Useful for tail-call dispatch loops, where every handler
/// ends by tail-calling the next.
#if __has_cpp_attribute(clang::preserve_none)
#define FOLLY_ATTR_CLANG_PRESERVE_NONE clang::preserve_none
#else
#define FOLLY_ATTR_CLANG_PRESERVE_NONE
#endif

#if __has_cpp_attribute(gnu::used)
#define FOLLY_ATTR_GNU_USED gnu::used
#else
#define FOLLY_ATTR_GNU_USED
#endif

#if __has_cpp_attribute(gnu::retain)
#define FOLLY_ATTR_GNU_RETAIN gnu::retain
#else
#define FOLLY_ATTR_GNU_RETAIN
#endif

#if __has_cpp_attribute(gnu::noclone)
#define FOLLY_ATTR_GNU_NOCLONE gnu::noclone
#else
#define FOLLY_ATTR_GNU_NOCLONE
#endif

#if __has_cpp_attribute(gnu::flatten)
#define FOLLY_ATTR_GNU_FLATTEN gnu::flatten
#else
#define FOLLY_ATTR_GNU_FLATTEN
#endif

#if __has_cpp_attribute(clang::lifetimebound)
#define FOLLY_ATTR_CLANG_LIFETIMEBOUND clang::lifetimebound
#else
#define FOLLY_ATTR_CLANG_LIFETIMEBOUND
#endif

#if __has_cpp_attribute(clang::coro_await_elidable)
#define FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE clang::coro_await_elidable
#else
#define FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE
#endif

#if __has_cpp_attribute(clang::coro_await_elidable_argument)
#define FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE_ARGUMENT \
  clang::coro_await_elidable_argument
#else
#define FOLLY_ATTR_CLANG_CORO_AWAIT_ELIDABLE_ARGUMENT
#endif

#if __has_cpp_attribute(clang::no_thread_safety_analysis)
#define FOLLY_ATTR_CLANG_NO_THREAD_SAFETY_ANALYSIS \
  clang::no_thread_safety_analysis
#else
#define FOLLY_ATTR_CLANG_NO_THREAD_SAFETY_ANALYSIS
#endif

/**
 * FOLLY_ATTR_MUSTTAIL requests that the compiler emit a tail call for the
 * annotated return statement. When the attribute is supported (Clang, GCC 15+,
 * MSVC), the compiler will enforce tail-call emission and produce a compilation
 * error if a tail call cannot be generated. On compilers that do not support
 * the attribute, the macro expands to nothing and tail-call optimization is
 * not guaranteed.
 *
 *  int factorial_impl(int n, int acc) {
 *    if (n <= 1) return acc;
 *    FOLLY_ATTR_MUSTTAIL return factorial_impl(n - 1, n * acc);
 *  }
 */
#if __has_cpp_attribute(gnu::musttail)
#define FOLLY_ATTR_MUSTTAIL [[gnu::musttail]]
#elif __has_cpp_attribute(clang::musttail)
#define FOLLY_ATTR_MUSTTAIL [[clang::musttail]]
#elif __has_cpp_attribute(msvc::musttail)
#define FOLLY_ATTR_MUSTTAIL [[msvc::musttail]]
#else
#define FOLLY_ATTR_MUSTTAIL
#endif
