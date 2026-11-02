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

/* These definitions are in a separate file so that they
 * may be included from C- as well as C++-based projects. */

#include <folly/portability/Config.h>

/**
 * Portable version check.
 */
#ifndef __GNUC_PREREQ
#if defined __GNUC__ && defined __GNUC_MINOR__
/* nolint */
#define __GNUC_PREREQ(maj, min) \
  ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
#else
/* nolint */
#define __GNUC_PREREQ(maj, min) 0
#endif
#endif

// portable version check for clang
#ifndef __CLANG_PREREQ
#if defined __clang__ && defined __clang_major__ && defined __clang_minor__
/* nolint */
#define __CLANG_PREREQ(maj, min) \
  ((__clang_major__ << 16) + __clang_minor__ >= ((maj) << 16) + (min))
#else
/* nolint */
#define __CLANG_PREREQ(maj, min) 0
#endif
#endif

/// FOLLY_GLIBC_PREREQ
#if !defined(__GLIBC__)
#define FOLLY_GLIBC_PREREQ(maj, min) 0
#else
#define FOLLY_GLIBC_PREREQ(maj, min) \
  (__GLIBC__ > (maj)) || (__GLIBC__ == (maj) && __GLIBC_MINOR__ >= (min))
#endif

#if defined(__has_builtin)
#define FOLLY_HAS_BUILTIN(...) __has_builtin(__VA_ARGS__)
#else
#define FOLLY_HAS_BUILTIN(...) 0
#endif

#if defined(__has_feature)
#define FOLLY_HAS_FEATURE(...) __has_feature(__VA_ARGS__)
#else
#define FOLLY_HAS_FEATURE(...) 0
#endif

#if defined(__has_warning)
#define FOLLY_HAS_WARNING(...) __has_warning(__VA_ARGS__)
#else
#define FOLLY_HAS_WARNING(...) 0
#endif

/* FOLLY_SANITIZE_ADDRESS is defined to 1 if the current compilation unit
 * is being compiled with ASAN or HWASAN enabled.
 *
 * Beware when using this macro in a header file: this macro may change values
 * across compilation units if some libraries are built with ASAN/HWASAN enabled
 * and some built with ASAN/HWSAN disabled. For instance, this may occur, if
 * folly itself was compiled without ASAN/HWSAN but a downstream project that
 * uses folly is compiling with ASAN/HWSAN enabled.
 *
 * Use FOLLY_LIBRARY_SANITIZE_ADDRESS (defined in folly-config.h) to check if
 * folly itself was compiled with ASAN enabled.
 */
#ifndef FOLLY_SANITIZE_ADDRESS
#if FOLLY_HAS_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__) || \
    FOLLY_HAS_FEATURE(hwaddress_sanitizer)
#define FOLLY_SANITIZE_ADDRESS 1
#endif
#endif

/* Define attribute wrapper for function attribute used to disable
 * address sanitizer instrumentation. Unfortunately, this attribute
 * has issues when inlining is used, so disable that as well. */
#ifdef FOLLY_SANITIZE_ADDRESS
#if defined(__clang__)
#if __has_attribute(__no_sanitize__)
#define FOLLY_DISABLE_ADDRESS_SANITIZER                     \
  __attribute__((__no_sanitize__("address"), __noinline__)) \
  __attribute__((__no_sanitize__("hwaddress"), __noinline__))
#elif __has_attribute(__no_address_safety_analysis__)
#define FOLLY_DISABLE_ADDRESS_SANITIZER \
  __attribute__((__no_address_safety_analysis__, __noinline__))
#elif __has_attribute(__no_sanitize_address__)
#define FOLLY_DISABLE_ADDRESS_SANITIZER \
  __attribute__((__no_sanitize_address__, __noinline__))
#endif
#elif defined(__GNUC__)
#define FOLLY_DISABLE_ADDRESS_SANITIZER \
  __attribute__((__no_address_safety_analysis__, __noinline__))
#elif defined(_MSC_VER)
#define FOLLY_DISABLE_ADDRESS_SANITIZER __declspec(no_sanitize_address)
#endif
#endif
#ifndef FOLLY_DISABLE_ADDRESS_SANITIZER
#define FOLLY_DISABLE_ADDRESS_SANITIZER
#endif

/* Define a convenience macro to test when thread sanitizer is being used
 * across the different compilers (e.g. clang, gcc) */
#ifndef FOLLY_SANITIZE_THREAD
#if FOLLY_HAS_FEATURE(thread_sanitizer) || defined(__SANITIZE_THREAD__)
#define FOLLY_SANITIZE_THREAD 1
#endif
#endif

#ifdef FOLLY_SANITIZE_THREAD
#define FOLLY_DISABLE_THREAD_SANITIZER \
  __attribute__((no_sanitize_thread, noinline))
#else
#define FOLLY_DISABLE_THREAD_SANITIZER
#endif

/**
 * Define a convenience macro to test when memory sanitizer is being used
 * across the different compilers (e.g. clang, gcc)
 */
#ifndef FOLLY_SANITIZE_MEMORY
#if FOLLY_HAS_FEATURE(memory_sanitizer) || defined(__SANITIZE_MEMORY__)
#define FOLLY_SANITIZE_MEMORY 1
#endif
#endif

#ifdef FOLLY_SANITIZE_MEMORY
#define FOLLY_DISABLE_MEMORY_SANITIZER \
  __attribute__((no_sanitize_memory, noinline))
#else
#define FOLLY_DISABLE_MEMORY_SANITIZER
#endif

/**
 * Define a convenience macro to test when dataflow sanitizer is being used
 * across the different compilers (e.g. clang, gcc)
 */
#ifndef FOLLY_SANITIZE_DATAFLOW
#if FOLLY_HAS_FEATURE(dataflow_sanitizer) || defined(__SANITIZE_DATAFLOW__)
#define FOLLY_SANITIZE_DATAFLOW 1
#endif
#endif

#ifdef FOLLY_SANITIZE_DATAFLOW
#define FOLLY_DISABLE_DATAFLOW_SANITIZER \
  __attribute__((no_sanitize_dataflow, noinline))
#else
#define FOLLY_DISABLE_DATAFLOW_SANITIZER
#endif

/**
 * Define a convenience macro to test when undefined-behavior sanitizer is being
 * used across the different compilers (e.g. clang, gcc)
 */
#ifndef FOLLY_SANITIZE_UNDEFINED_BEHAVIOR
#if FOLLY_HAS_FEATURE(undefined_behavior_sanitizer) || \
    defined(__SANITIZER_UNDEFINED__)
#define FOLLY_SANITIZE_UNDEFINED_BEHAVIOR(...) 1
#endif
#endif

#ifdef FOLLY_SANITIZE_UNDEFINED_BEHAVIOR
#define FOLLY_DISABLE_UNDEFINED_BEHAVIOR_SANITIZER(...) \
  __attribute__((no_sanitize(__VA_ARGS__)))
#else
#define FOLLY_DISABLE_UNDEFINED_BEHAVIOR_SANITIZER(...)
#endif

/**
 * Define a convenience macro to test when ASAN, UBSAN, TSAN or MSAN sanitizer
 * are being used
 */
#ifndef FOLLY_SANITIZE
#if defined(FOLLY_SANITIZE_ADDRESS) || defined(FOLLY_SANITIZE_THREAD) ||  \
    defined(FOLLY_SANITIZE_MEMORY) || defined(FOLLY_SANITIZE_DATAFLOW) || \
    defined(FOLLY_SANITIZE_UNDEFINED_BEHAVIOR)
#define FOLLY_SANITIZE 1
#endif
#endif

#define FOLLY_DISABLE_SANITIZERS  \
  FOLLY_DISABLE_ADDRESS_SANITIZER \
  FOLLY_DISABLE_THREAD_SANITIZER  \
  FOLLY_DISABLE_MEMORY_SANITIZER  \
  FOLLY_DISABLE_UNDEFINED_BEHAVIOR_SANITIZER("undefined")

/**
 * Macro for marking functions as having public visibility.
 */
#if defined(__GNUC__)
#define FOLLY_EXPORT __attribute__((__visibility__("default")))
#else
#define FOLLY_EXPORT
#endif

// noinline
#ifdef _MSC_VER
#define FOLLY_NOINLINE __declspec(noinline)
#elif defined(__HIP_PLATFORM_HCC__) || defined(__CUDA__)
// HIP and CUDA headers define their own __noinline__ macro which conflicts
// with the __attribute__((__noinline__)) spelling.
#define FOLLY_NOINLINE __attribute__((noinline))
#elif defined(__GNUC__)
#define FOLLY_NOINLINE __attribute__((__noinline__))
#else
#define FOLLY_NOINLINE
#endif

// always inline
#ifdef _MSC_VER
#define FOLLY_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__)
#define FOLLY_ALWAYS_INLINE inline __attribute__((__always_inline__))
#else
#define FOLLY_ALWAYS_INLINE inline
#endif

// attribute hidden
#if defined(_MSC_VER)
#define FOLLY_ATTR_VISIBILITY_HIDDEN
#elif defined(__GNUC__)
#define FOLLY_ATTR_VISIBILITY_HIDDEN __attribute__((__visibility__("hidden")))
#else
#define FOLLY_ATTR_VISIBILITY_HIDDEN
#endif

// An attribute for marking symbols as weak, if supported
#if FOLLY_HAVE_WEAK_SYMBOLS
#define FOLLY_ATTR_WEAK __attribute__((__weak__))
#else
#define FOLLY_ATTR_WEAK
#endif

#if defined(__has_attribute)
#if __has_attribute(weak)
#define FOLLY_ATTR_WEAK_SYMBOLS_COMPILE_TIME __attribute__((__weak__))
#else
#define FOLLY_ATTR_WEAK_SYMBOLS_COMPILE_TIME
#endif
#else
#define FOLLY_ATTR_WEAK_SYMBOLS_COMPILE_TIME
#endif

// Microsoft ABI version (can be overridden manually if necessary)
#ifndef FOLLY_MICROSOFT_ABI_VER
#ifdef _MSC_VER
#define FOLLY_MICROSOFT_ABI_VER _MSC_VER
#endif
#endif

// FOLLY_NAME_RESOLVABLE
//
// An attribute that marks a function or variable as needing to be resolvable
// by name. This generally is needed if inline assembly refers to the variable
// by string name.
#ifdef __roar__
#define FOLLY_NAME_RESOLVABLE __attribute__((roar_resolvable_by_name))
#else
#define FOLLY_NAME_RESOLVABLE
#endif

//  FOLLY_ERASE
//
//  A conceptual attribute/syntax combo for erasing a function from the build
//  artifacts and forcing all call-sites to inline the callee, at least as far
//  as each compiler supports.
//
//  Semantically includes the inline specifier.
#define FOLLY_ERASE FOLLY_ALWAYS_INLINE FOLLY_ATTR_VISIBILITY_HIDDEN

//  FOLLY_ERASE_NOINLINE
//
//  Like FOLLY_ERASE, but also noinline. The naming similarity with FOLLY_ERASE
//  is specifically desirable.
#define FOLLY_ERASE_NOINLINE FOLLY_NOINLINE FOLLY_ATTR_VISIBILITY_HIDDEN

//  FOLLY_ERASE_HACK_GCC
//
//  Equivalent to FOLLY_ERASE, but without hiding under gcc. Useful when applied
//  to a function which may sometimes be hidden separately, for example by being
//  declared in an anonymous namespace, since in such cases with -Wattributes
//  enabled, gcc would emit: 'visibility' attribute ignored.
//
//  Semantically includes the inline specifier.
#if defined(__GNUC__) && !defined(__clang__)
#define FOLLY_ERASE_HACK_GCC FOLLY_ALWAYS_INLINE
#else
#define FOLLY_ERASE_HACK_GCC FOLLY_ERASE
#endif

//  FOLLY_ERASE_TRYCATCH
//
//  Equivalent to FOLLY_ERASE, but for code which might contain explicit
//  exception handling. Has the effect of FOLLY_ERASE, except under MSVC which
//  warns about __forceinline when functions contain exception handling.
//
//  Semantically includes the inline specifier.
#ifdef _MSC_VER
#define FOLLY_ERASE_TRYCATCH inline
#else
#define FOLLY_ERASE_TRYCATCH FOLLY_ERASE
#endif

// Generalize warning push/pop.
#if defined(__GNUC__) || defined(__clang__)
// Clang & GCC
#define FOLLY_PUSH_WARNING _Pragma("GCC diagnostic push")
#define FOLLY_POP_WARNING _Pragma("GCC diagnostic pop")
#define FOLLY_GNU_ENABLE_WARNING_INTERNAL2(warningName) #warningName
#define FOLLY_GNU_DISABLE_WARNING_INTERNAL2(warningName) #warningName
#define FOLLY_GNU_ENABLE_ERROR_INTERNAL2(warningName) #warningName
#define FOLLY_GNU_DISABLE_WARNING(warningName) \
  _Pragma(                                     \
      FOLLY_GNU_DISABLE_WARNING_INTERNAL2(GCC diagnostic ignored warningName))
#define FOLLY_GNU_ENABLE_WARNING(warningName) \
  _Pragma(                                    \
      FOLLY_GNU_ENABLE_WARNING_INTERNAL2(GCC diagnostic warning warningName))
#define FOLLY_GNU_ENABLE_ERROR(warningName) \
  _Pragma(FOLLY_GNU_ENABLE_ERROR_INTERNAL2(GCC diagnostic error warningName))
#ifdef __clang__
#define FOLLY_CLANG_DISABLE_WARNING(warningName) \
  FOLLY_GNU_DISABLE_WARNING(warningName)
#define FOLLY_GCC_DISABLE_WARNING(warningName)
#else
#define FOLLY_CLANG_DISABLE_WARNING(warningName)
#define FOLLY_GCC_DISABLE_WARNING(warningName) \
  FOLLY_GNU_DISABLE_WARNING(warningName)
#endif
#define FOLLY_MSVC_DISABLE_WARNING(warningNumber)
#elif defined(_MSC_VER)
#define FOLLY_PUSH_WARNING __pragma(warning(push))
#define FOLLY_POP_WARNING __pragma(warning(pop))
// Disable the GCC warnings.
#define FOLLY_GNU_ENABLE_WARNING(warningName)
#define FOLLY_GNU_DISABLE_WARNING(warningName)
#define FOLLY_GNU_ENABLE_ERROR(warningName)
#define FOLLY_GCC_DISABLE_WARNING(warningName)
#define FOLLY_CLANG_DISABLE_WARNING(warningName)
#define FOLLY_MSVC_DISABLE_WARNING(warningNumber) \
  __pragma(warning(disable : warningNumber))
#else
#define FOLLY_PUSH_WARNING
#define FOLLY_POP_WARNING
#define FOLLY_GNU_ENABLE_WARNING(warningName)
#define FOLLY_GNU_DISABLE_WARNING(warningName)
#define FOLLY_GNU_ENABLE_ERROR(warningName)
#define FOLLY_GCC_DISABLE_WARNING(warningName)
#define FOLLY_CLANG_DISABLE_WARNING(warningName)
#define FOLLY_MSVC_DISABLE_WARNING(warningNumber)
#endif

// Mark a region that intentionally uses raw pointer arithmetic, unchecked
// buffer indexing, or unsafe libc buffer functions, exempting it from
// -Wunsafe-buffer-usage. Regions cannot nest and must begin and end in the
// same file.
#if defined(__clang__) && FOLLY_HAS_WARNING("-Wunsafe-buffer-usage")
#define FOLLY_UNSAFE_BUFFER_USAGE_BEGIN \
  _Pragma("clang unsafe_buffer_usage begin")
#define FOLLY_UNSAFE_BUFFER_USAGE_END _Pragma("clang unsafe_buffer_usage end")
#else
#define FOLLY_UNSAFE_BUFFER_USAGE_BEGIN
#define FOLLY_UNSAFE_BUFFER_USAGE_END
#endif

#ifdef FOLLY_HAVE_SHADOW_LOCAL_WARNINGS
#define FOLLY_GCC_DISABLE_NEW_SHADOW_WARNINGS            \
  FOLLY_GNU_DISABLE_WARNING("-Wshadow-compatible-local") \
  FOLLY_GNU_DISABLE_WARNING("-Wshadow-local")            \
  FOLLY_GNU_DISABLE_WARNING("-Wshadow")
#else
#define FOLLY_GCC_DISABLE_NEW_SHADOW_WARNINGS /* empty */
#endif

#if defined(_MSC_VER)
#define FOLLY_MSVC_DECLSPEC(...) __declspec(__VA_ARGS__)
#else
#define FOLLY_MSVC_DECLSPEC(...)
#endif
