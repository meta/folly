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

#include <stdlib.h>

#include <folly/CPortability.h>
#include <folly/portability/Config.h>

#if (defined(USE_JEMALLOC) || defined(FOLLY_USE_JEMALLOC)) && \
    !defined(FOLLY_SANITIZE)
#if defined(FOLLY_ASSUME_NO_JEMALLOC)
#error \
    "Both USE_JEMALLOC/FOLLY_USE_JEMALLOC and FOLLY_ASSUME_NO_JEMALLOC defined"
#endif
// JEMalloc provides its own implementation of
// malloc_usable_size, and that's what we should be using.
#if defined(__FreeBSD__)
#include <malloc_np.h> // @manual
#define FOLLY_HAS_JEMALLOC_DEFS 0
#else
// Keep jemalloc's stable API aliases without remapping malloc/free macros.
#ifndef JEMALLOC_NO_DEMANGLE
#define JEMALLOC_NO_DEMANGLE
#endif
#include <jemalloc/jemalloc.h> // @manual
#define FOLLY_HAS_JEMALLOC_DEFS 1

#ifdef USE_JEMALLOC
// Explicit jemalloc builds can bind its helpers directly. Leave the weak-symbol
// path unchanged when the allocator is selected at runtime (e.g. TCMalloc).
namespace folly {
inline constexpr auto malloc_usable_size = ::je_malloc_usable_size;
inline constexpr auto mallocx = ::je_mallocx;
inline constexpr auto rallocx = ::je_rallocx;
inline constexpr auto xallocx = ::je_xallocx;
inline constexpr auto sallocx = ::je_sallocx;
inline constexpr auto dallocx = ::je_dallocx;
inline constexpr auto sdallocx = ::je_sdallocx;
inline constexpr auto nallocx = ::je_nallocx;
inline constexpr auto mallctl = ::je_mallctl;
inline constexpr auto mallctlnametomib = ::je_mallctlnametomib;
inline constexpr auto mallctlbymib = ::je_mallctlbymib;
#ifdef je_free_aligned_sized
inline constexpr auto free_aligned_sized = ::je_free_aligned_sized;
#endif
} // namespace folly
#endif
#endif
#else
#if !defined(__FreeBSD__)
#if __has_include(<malloc.h>)
#include <malloc.h>
#endif
#define FOLLY_HAS_JEMALLOC_DEFS 0
#endif

#if defined(__APPLE__) && !defined(FOLLY_HAVE_MALLOC_USABLE_SIZE)
// MacOS doesn't have malloc_usable_size()
extern "C" size_t malloc_usable_size(void* ptr);
#elif defined(_WIN32)
extern "C" size_t malloc_usable_size(void* ptr);
#endif
#endif
