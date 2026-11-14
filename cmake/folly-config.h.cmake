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

#ifdef __APPLE__
#include <TargetConditionals.h> // @manual
#endif

#if !defined(FOLLY_MOBILE)
#if defined(__ANDROID__) || \
    (defined(__APPLE__) &&  \
     (TARGET_IPHONE_SIMULATOR || TARGET_OS_SIMULATOR || TARGET_OS_IPHONE))
#define FOLLY_MOBILE 1
#else
#define FOLLY_MOBILE 0
#endif
#endif // FOLLY_MOBILE

#cmakedefine01 FOLLY_HAVE_PTHREAD
#cmakedefine FOLLY_HAVE_PTHREAD_ATFORK 1

#cmakedefine FOLLY_HAVE_LIBGFLAGS 1

#cmakedefine FOLLY_HAVE_LIBGLOG 1

#cmakedefine FOLLY_USE_JEMALLOC 1

#if __has_include(<features.h>)
#include <features.h>
#endif

#cmakedefine FOLLY_HAVE_ACCEPT4 1
#cmakedefine01 FOLLY_HAVE_GETRANDOM
#cmakedefine FOLLY_HAVE_PREADV 1
#cmakedefine FOLLY_HAVE_PWRITEV 1
#cmakedefine01 FOLLY_HAVE_CLOCK_GETTIME
#cmakedefine FOLLY_HAVE_PIPE2 1

#cmakedefine FOLLY_HAVE_IFUNC 1
#cmakedefine FOLLY_HAVE_UNALIGNED_ACCESS 1
#cmakedefine FOLLY_HAVE_VLA 1
#cmakedefine01 FOLLY_HAVE_WEAK_SYMBOLS
#cmakedefine FOLLY_HAVE_LINUX_VDSO 1
#cmakedefine FOLLY_HAVE_MALLOC_USABLE_SIZE 1
#cmakedefine FOLLY_HAVE_INT128_T 1
#cmakedefine FOLLY_HAVE_WCHAR_SUPPORT 1
#cmakedefine FOLLY_HAVE_EXTRANDOM_SFMT19937 1
#cmakedefine HAVE_VSNPRINTF_ERRORS 1

#cmakedefine FOLLY_HAVE_LIBUNWIND 1
#cmakedefine01 FOLLY_HAVE_DWARF
#cmakedefine01 FOLLY_HAVE_ELF
#cmakedefine FOLLY_HAVE_SWAPCONTEXT 1
#cmakedefine FOLLY_FIBERS_USE_BOOST_CONTEXT 1
#cmakedefine FOLLY_HAVE_BACKTRACE 1
#cmakedefine FOLLY_USE_SYMBOLIZER 1
#define FOLLY_DEMANGLE_MAX_SYMBOL_SIZE 1024

#cmakedefine FOLLY_HAVE_SHADOW_LOCAL_WARNINGS 1

#cmakedefine01 FOLLY_HAVE_LIBLZ4
#cmakedefine01 FOLLY_HAVE_LIBLZMA
#cmakedefine01 FOLLY_HAVE_LIBSNAPPY
#cmakedefine01 FOLLY_HAVE_LIBZ
#cmakedefine01 FOLLY_HAVE_LIBZSTD
#cmakedefine01 FOLLY_HAVE_LIBBZ2

// The CMake build neither finds nor links BLAKE3 or xxHash, so the
// UniqueHashKey algorithms that need them are left out.
#define FOLLY_HAVE_BLAKE3 0
#define FOLLY_HAVE_XXH3 0

#cmakedefine01 FOLLY_HAVE_OPENSSL

#cmakedefine01 FOLLY_LIBRARY_SANITIZE_ADDRESS

#cmakedefine FOLLY_SUPPORT_SHARED_LIBRARY 1

#cmakedefine01 FOLLY_HAVE_LIBRT

#cmakedefine01 FOLLY_HAVE_VSOCK
