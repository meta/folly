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
#include <cstdint>

#include <folly/CPortability.h>
#include <folly/portability/Config.h>

#if defined(_MSC_VER)
#define FOLLY_CPLUSPLUS _MSVC_LANG
#else
#define FOLLY_CPLUSPLUS __cplusplus
#endif

// On MSVC an incorrect <version> header get's picked up
#if !defined(_MSC_VER) && __has_include(<version>)
#include <version>
#endif

static_assert(FOLLY_CPLUSPLUS >= 202002L, "__cplusplus >= 202002L");

#if defined(__GNUC__) && !defined(__clang__)
#if defined(FOLLY_CONFIG_TEMPORARY_DOWNGRADE_GCC)
static_assert(__GNUC__ >= 9, "__GNUC__ >= 9");
#else
static_assert(__GNUC__ >= 10, "__GNUC__ >= 10");
#endif
#endif

#if defined(_MSC_VER)
static_assert(_MSC_VER >= 1920);
#endif

#if defined(_MSC_VER) || defined(_CPPLIB_VER)
static_assert(FOLLY_CPLUSPLUS >= 202002L, "__cplusplus >= 202002L");
#endif

// Unaligned loads and stores
namespace folly {
#if defined(FOLLY_HAVE_UNALIGNED_ACCESS) && FOLLY_HAVE_UNALIGNED_ACCESS
constexpr bool kHasUnalignedAccess = true;
#else
constexpr bool kHasUnalignedAccess = false;
#endif
} // namespace folly

// compiler specific attribute translation
// msvc should come first, so if clang is in msvc mode it gets the right defines

// NOTE: this will only do checking in msvc with versions that support /analyze
#ifdef _MSC_VER
#ifdef _USE_ATTRIBUTES_FOR_SAL
#undef _USE_ATTRIBUTES_FOR_SAL
#endif
/* nolint */
#define _USE_ATTRIBUTES_FOR_SAL 1
#include <sal.h> // @manual
#define FOLLY_PRINTF_FORMAT _Printf_format_string_
#define FOLLY_PRINTF_FORMAT_ATTR(format_param, dots_param) /**/
#else
#define FOLLY_PRINTF_FORMAT /**/
#define FOLLY_PRINTF_FORMAT_ATTR(format_param, dots_param) \
  __attribute__((__format__(__printf__, format_param, dots_param)))
#endif

// older clang-format gets confused by [[deprecated(...)]] on class decls
#define FOLLY_DEPRECATED(...) [[deprecated(__VA_ARGS__)]]

// target
#ifdef _MSC_VER
#define FOLLY_TARGET_ATTRIBUTE(target)
#else
#define FOLLY_TARGET_ATTRIBUTE(target) __attribute__((__target__(target)))
#endif

#if defined(__i386__) || defined(__i686__) || defined(__x86__) || \
    defined(_M_IX86)
#define FOLLY_X86 1
#else
#define FOLLY_X86 0
#endif

// detection for 64 bit
#if defined(__x86_64__) || defined(_M_X64)
#define FOLLY_X64 1
#else
#define FOLLY_X64 0
#endif

#if (FOLLY_X86 || FOLLY_X64) && defined(__AVX2__)
#define FOLLY_AVX2 1
#else
#define FOLLY_AVX2 0
#endif

#if defined(__arm__)
#define FOLLY_ARM 1
#else
#define FOLLY_ARM 0
#endif

#if defined(__aarch64__)
#define FOLLY_AARCH64 1
#else
#define FOLLY_AARCH64 0
#endif

#if defined(__aarch64__) && defined(__ARM_FEATURE_ATOMICS)
#define FOLLY_AARCH64_LSE 1
#else
#define FOLLY_AARCH64_LSE 0
#endif

// ACLE has no LSE2 feature macro. Armv9 guarantees it; Armv8.4+ targets
// are conservatively left unrecognized.
#if FOLLY_AARCH64 && defined(__ARM_ARCH) && __ARM_ARCH >= 9
#define FOLLY_AARCH64_LSE2 1
#else
#define FOLLY_AARCH64_LSE2 0
#endif

#if FOLLY_AARCH64 && defined(__ARM_FEATURE_RCPC) && __ARM_FEATURE_RCPC >= 3
#define FOLLY_AARCH64_LRCPC3 1
#else
#define FOLLY_AARCH64_LRCPC3 0
#endif

#if defined(__powerpc64__)
#define FOLLY_PPC64 1
#else
#define FOLLY_PPC64 0
#endif

#if defined(__s390x__)
#define FOLLY_S390X 1
#else
#define FOLLY_S390X 0
#endif

#if defined(__riscv)
#define FOLLY_RISCV64 1
#else
#define FOLLY_RISCV64 0
#endif

#if defined(__riscv_xlen)
#define FOLLY_RISCV_XLEN __riscv_xlen
#else
#define FOLLY_RISCV_XLEN 0
#endif

#if defined(__riscv) && (defined(__riscv_zaamo) || defined(__riscv_atomic))
#define FOLLY_RISCV_ZAAMO 1
#else
#define FOLLY_RISCV_ZAAMO 0
#endif

#if defined(__riscv) && defined(__riscv_zabha)
#define FOLLY_RISCV_ZABHA 1
#else
#define FOLLY_RISCV_ZABHA 0
#endif

//  The riscv Zbb (basic bit-manipulation) extension.
#if defined(__riscv) && defined(__riscv_zbb)
#define FOLLY_RISCV_ZBB 1
#else
#define FOLLY_RISCV_ZBB 0
#endif

#if defined(__wasm__)
#define FOLLY_WASM 1
#else
#define FOLLY_WASM 0
#endif

#if defined(__wasm32__)
#define FOLLY_WASM32 1
#else
#define FOLLY_WASM32 0
#endif

#if defined(__wasm64__)
#define FOLLY_WASM64 1
#else
#define FOLLY_WASM64 0
#endif

namespace folly {
constexpr bool kIsArchArm = FOLLY_ARM == 1;
constexpr bool kIsArchX86 = FOLLY_X86 == 1;
constexpr bool kIsArchAmd64 = FOLLY_X64 == 1;
constexpr bool kHasAvx2 = FOLLY_AVX2 == 1;
constexpr bool kIsArchAArch64 = FOLLY_AARCH64 == 1;
constexpr bool kIsArchAArch64Lse = FOLLY_AARCH64_LSE == 1;
constexpr bool kIsArchAArch64Lse2 = FOLLY_AARCH64_LSE2 == 1;
constexpr bool kIsArchAArch64Lrcpc3 = FOLLY_AARCH64_LRCPC3 == 1;
constexpr bool kIsArchPPC64 = FOLLY_PPC64 == 1;
constexpr bool kIsArchS390X = FOLLY_S390X == 1;
constexpr bool kIsArchRISCV64 = FOLLY_RISCV64 == 1;
constexpr unsigned kArchRISCVXlen = FOLLY_RISCV_XLEN;
constexpr bool kIsArchRISCVZaamo = FOLLY_RISCV_ZAAMO == 1;
constexpr bool kIsArchRISCVZabha = FOLLY_RISCV_ZABHA == 1;
constexpr bool kIsArchRISCVZbb = FOLLY_RISCV_ZBB == 1;
constexpr bool kIsArchWasm = FOLLY_WASM == 1;
constexpr bool kIsArchWasm32 = FOLLY_WASM32 == 1;
constexpr bool kIsArchWasm64 = FOLLY_WASM64 == 1;
} // namespace folly

namespace folly {

/**
 * folly::kIsLibrarySanitizeAddress reports if folly was compiled with ASAN
 * enabled.  Note that for compilation units outside of folly that include
 * folly/Portability.h, the value of kIsLibrarySanitizeAddress may be different
 * from whether or not the current compilation unit is being compiled with ASAN.
 */
#if FOLLY_LIBRARY_SANITIZE_ADDRESS
constexpr bool kIsLibrarySanitizeAddress = true;
#else
constexpr bool kIsLibrarySanitizeAddress = false;
#endif

#ifdef FOLLY_SANITIZE_ADDRESS
constexpr bool kIsSanitizeAddress = true;
#else
constexpr bool kIsSanitizeAddress = false;
#endif

#ifdef FOLLY_SANITIZE_THREAD
constexpr bool kIsSanitizeThread = true;
#else
constexpr bool kIsSanitizeThread = false;
#endif

#ifdef FOLLY_SANITIZE_DATAFLOW
constexpr bool kIsSanitizeDataflow = true;
#else
constexpr bool kIsSanitizeDataflow = false;
#endif

#ifdef FOLLY_SANITIZE
constexpr bool kIsSanitize = true;
#else
constexpr bool kIsSanitize = false;
#endif

#if defined(__OPTIMIZE__)
constexpr bool kIsOptimize = true;
#else
constexpr bool kIsOptimize = false;
#endif

#if defined(__OPTIMIZE_SIZE__)
constexpr bool kIsOptimizeSize = true;
#else
constexpr bool kIsOptimizeSize = false;
#endif
} // namespace folly

// packing is very ugly in msvc
#ifdef _MSC_VER
#define FOLLY_PACK_ATTR /**/
#define FOLLY_PACK_PUSH __pragma(pack(push, 1))
#define FOLLY_PACK_POP __pragma(pack(pop))
#elif defined(__GNUC__)
#define FOLLY_PACK_ATTR __attribute__((__packed__))
#define FOLLY_PACK_PUSH /**/
#define FOLLY_PACK_POP /**/
#else
#define FOLLY_PACK_ATTR /**/
#define FOLLY_PACK_PUSH /**/
#define FOLLY_PACK_POP /**/
#endif

// It turns out that GNU libstdc++ and LLVM libc++ differ on how they implement
// the 'std' namespace; the latter uses inline namespaces. Wrap this decision
// up in a macro to make forward-declarations easier.
#if defined(_LIBCPP_VERSION)
#define FOLLY_NAMESPACE_STD_BEGIN _LIBCPP_BEGIN_NAMESPACE_STD
#define FOLLY_NAMESPACE_STD_END _LIBCPP_END_NAMESPACE_STD
#else
#define FOLLY_NAMESPACE_STD_BEGIN namespace std {
#define FOLLY_NAMESPACE_STD_END }
#endif

// If the new c++ ABI is used, __cxx11 inline namespace needs to be added to
// some types, e.g. std::list.
#if defined(_GLIBCXX_USE_CXX11_ABI) && _GLIBCXX_USE_CXX11_ABI
#define FOLLY_GLIBCXX_NAMESPACE_CXX11_BEGIN \
  inline _GLIBCXX_BEGIN_NAMESPACE_CXX11
#define FOLLY_GLIBCXX_NAMESPACE_CXX11_END _GLIBCXX_END_NAMESPACE_CXX11
#else
#define FOLLY_GLIBCXX_NAMESPACE_CXX11_BEGIN
#define FOLLY_GLIBCXX_NAMESPACE_CXX11_END
#endif

// MSVC specific defines
// mainly for posix compat
#ifdef _MSC_VER

// We have compiler support for the newest of the new, but
// MSVC doesn't tell us that.
//
// Clang pretends to be MSVC on Windows, but it refuses to compile
// SSE4.2 intrinsics unless -march argument is specified.
// So cannot unconditionally define __SSE4_2__ in clang.
#ifndef __clang__
#if !defined(_M_ARM) && !defined(_M_ARM64)
#define __SSE4_2__ 1
#endif // !defined(_M_ARM) && !defined(_M_ARM64)

// Hide a GCC specific thing that breaks MSVC if left alone.
#define __extension__

// compiler specific to compiler specific
// nolint
#define __PRETTY_FUNCTION__ __FUNCSIG__
#endif

#endif

// Define FOLLY_HAS_EXCEPTIONS
#if (defined(__cpp_exceptions) && __cpp_exceptions >= 199711) || \
    FOLLY_HAS_FEATURE(cxx_exceptions)
#define FOLLY_HAS_EXCEPTIONS 1
#elif __GNUC__
#if defined(__EXCEPTIONS) && __EXCEPTIONS
#define FOLLY_HAS_EXCEPTIONS 1
#else // __EXCEPTIONS
#define FOLLY_HAS_EXCEPTIONS 0
#endif // __EXCEPTIONS
#elif FOLLY_MICROSOFT_ABI_VER
#if _CPPUNWIND
#define FOLLY_HAS_EXCEPTIONS 1
#else // _CPPUNWIND
#define FOLLY_HAS_EXCEPTIONS 0
#endif // _CPPUNWIND
#else
#define FOLLY_HAS_EXCEPTIONS 1 // default assumption for unknown platforms
#endif

// Debug
namespace folly {
#ifdef NDEBUG
constexpr auto kIsDebug = false;
#else
constexpr auto kIsDebug = true;
#endif
} // namespace folly

// Exceptions
namespace folly {
#if FOLLY_HAS_EXCEPTIONS
constexpr auto kHasExceptions = true;
#else
constexpr auto kHasExceptions = false;
#endif
} // namespace folly

// Endianness
namespace folly {
#ifdef _MSC_VER
// It's MSVC, so we just have to guess ... and allow an override
#ifdef FOLLY_ENDIAN_BE
constexpr auto kIsLittleEndian = false;
#else
constexpr auto kIsLittleEndian = true;
#endif
#else
constexpr auto kIsLittleEndian = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
#endif
constexpr auto kIsBigEndian = !kIsLittleEndian;
} // namespace folly

// Weak
namespace folly {
#if FOLLY_HAVE_WEAK_SYMBOLS
constexpr auto kHasWeakSymbols = true;
#else
constexpr auto kHasWeakSymbols = false;
#endif
} // namespace folly

#ifndef FOLLY_SSE
#if defined(__SSE4_2__)
#define FOLLY_SSE 4
#define FOLLY_SSE_MINOR 2
#elif defined(__SSE4_1__)
#define FOLLY_SSE 4
#define FOLLY_SSE_MINOR 1
#elif defined(__SSE4__)
#define FOLLY_SSE 4
#define FOLLY_SSE_MINOR 0
#elif defined(__SSE3__)
#define FOLLY_SSE 3
#define FOLLY_SSE_MINOR 0
#elif defined(__SSE2__)
#define FOLLY_SSE 2
#define FOLLY_SSE_MINOR 0
#elif defined(__SSE__)
#define FOLLY_SSE 1
#define FOLLY_SSE_MINOR 0
#else
#define FOLLY_SSE 0
#define FOLLY_SSE_MINOR 0
#endif
#endif

#ifndef FOLLY_SSSE
#if defined(__SSSE3__)
#define FOLLY_SSSE 3
#else
#define FOLLY_SSSE 0
#endif
#endif

#define FOLLY_SSE_PREREQ(major, minor) \
  (FOLLY_SSE > major || FOLLY_SSE == major && FOLLY_SSE_MINOR >= minor)

// A build that must not run AVX-512 code, even on a CPU that has it, sets
// FOLLY_DISABLE_AVX512 to 1. Defining DISABLE_AVX512, which other libraries
// also read, does the same.
#ifndef FOLLY_DISABLE_AVX512
#ifdef DISABLE_AVX512
#define FOLLY_DISABLE_AVX512 1
#else
#define FOLLY_DISABLE_AVX512 0
#endif
#endif

#ifndef FOLLY_NEON
#if (defined(__ARM_NEON) || defined(__ARM_NEON__)) && !defined(__CUDACC__)
#define FOLLY_NEON 1
#else
#define FOLLY_NEON 0
#endif
#endif

#ifndef FOLLY_ARM_FEATURE_CRC32
#ifdef __ARM_FEATURE_CRC32
#define FOLLY_ARM_FEATURE_CRC32 1
#else
#define FOLLY_ARM_FEATURE_CRC32 0
#endif
#endif

#ifndef FOLLY_ARM_FEATURE_CRYPTO
#ifdef __ARM_FEATURE_CRYPTO
#define FOLLY_ARM_FEATURE_CRYPTO 1
#else
#define FOLLY_ARM_FEATURE_CRYPTO 0
#endif
#endif

#ifndef FOLLY_ARM_FEATURE_AES
#ifdef __ARM_FEATURE_AES
#define FOLLY_ARM_FEATURE_AES 1
#else
#define FOLLY_ARM_FEATURE_AES 0
#endif
#endif

#ifndef FOLLY_ARM_FEATURE_SHA2
#ifdef __ARM_FEATURE_SHA2
#define FOLLY_ARM_FEATURE_SHA2 1
#else
#define FOLLY_ARM_FEATURE_SHA2 0
#endif
#endif

#ifndef FOLLY_ARM_FEATURE_SHA3
#ifdef __ARM_FEATURE_SHA3
#define FOLLY_ARM_FEATURE_SHA3 1
#else
#define FOLLY_ARM_FEATURE_SHA3 0
#endif
#endif

#ifndef FOLLY_ARM_FEATURE_SVE
#ifdef __ARM_FEATURE_SVE
#define FOLLY_ARM_FEATURE_SVE 1
#else
#define FOLLY_ARM_FEATURE_SVE 0
#endif
#endif

#ifndef FOLLY_ARM_FEATURE_SVE2
#ifdef __ARM_FEATURE_SVE2
#define FOLLY_ARM_FEATURE_SVE2 1
#else
#define FOLLY_ARM_FEATURE_SVE2 0
#endif
#endif

#ifndef FOLLY_ARM_FEATURE_NEON_SVE_BRIDGE
#if FOLLY_ARM_FEATURE_SVE && __has_include(<arm_neon_sve_bridge.h>)
#define FOLLY_ARM_FEATURE_NEON_SVE_BRIDGE 1
#else
#define FOLLY_ARM_FEATURE_NEON_SVE_BRIDGE 0
#endif
#endif

// RTTI may not be enabled for this compilation unit.
#if defined(__GXX_RTTI) || defined(__cpp_rtti) || \
    (defined(_MSC_VER) && defined(_CPPRTTI))
#define FOLLY_HAS_RTTI 1
#else
#define FOLLY_HAS_RTTI 0
#endif

namespace folly {
constexpr bool const kHasRtti = FOLLY_HAS_RTTI;
} // namespace folly

#if defined(__APPLE__) || defined(_MSC_VER)
#define FOLLY_STATIC_CTOR_PRIORITY_MAX
#else
// 101 is the highest priority allowed by the init_priority attribute.
// This priority is already used by JEMalloc and other memory allocators so
// we will take the next one.
#define FOLLY_STATIC_CTOR_PRIORITY_MAX __attribute__((__init_priority__(102)))
#endif

#if defined(__APPLE__) && TARGET_OS_IOS
#define FOLLY_APPLE_IOS 1
#else
#define FOLLY_APPLE_IOS 0
#endif

#if defined(__APPLE__) && TARGET_OS_OSX
#define FOLLY_APPLE_MACOS 1
#else
#define FOLLY_APPLE_MACOS 0
#endif

#if defined(__APPLE__) && TARGET_OS_TV
#define FOLLY_APPLE_TVOS 1
#else
#define FOLLY_APPLE_TVOS 0
#endif

#if defined(__APPLE__) && TARGET_OS_WATCH
#define FOLLY_APPLE_WATCHOS 1
#else
#define FOLLY_APPLE_WATCHOS 0
#endif

namespace folly {

#ifdef __OBJC__
constexpr auto kIsObjC = true;
#else
constexpr auto kIsObjC = false;
#endif

#if FOLLY_MOBILE
constexpr auto kIsMobile = true;
#else
constexpr auto kIsMobile = false;
#endif

#if defined(__linux__)
constexpr auto kIsLinuxActual = true;
#else
constexpr auto kIsLinuxActual = false;
#endif

constexpr auto kIsLinux = kIsLinuxActual && !kIsMobile;

// The Linux kernel never maps the first page of virtual address space, so any
// frame pointer below this threshold is a stale or corrupt value and must not
// be dereferenced.
#if defined(__linux__)
constexpr uintptr_t kMinValidAddress = 0x1000;
#else
constexpr uintptr_t kMinValidAddress = 0;
#endif

#if defined(__FreeBSD__)
constexpr auto kIsFreeBSD = true;
#else
constexpr auto kIsFreeBSD = false;
#endif

#if defined(_WIN32)
constexpr auto kIsWindows = true;
#else
constexpr auto kIsWindows = false;
#endif

#if defined(__ANDROID__)
constexpr auto kIsAndroid = true;
#else
constexpr auto kIsAndroid = false;
#endif

#if defined(__APPLE__)
constexpr auto kIsApple = true;
#else
constexpr auto kIsApple = false;
#endif

constexpr bool kIsAppleIOS = FOLLY_APPLE_IOS == 1;
constexpr bool kIsAppleMacOS = FOLLY_APPLE_MACOS == 1;
constexpr bool kIsAppleTVOS = FOLLY_APPLE_TVOS == 1;
constexpr bool kIsAppleWatchOS = FOLLY_APPLE_WATCHOS == 1;

/// Whether the target uses GNU libc, independent of its C++ standard library.
#if defined(__GLIBC__)
constexpr auto kIsGlibc = true;
#else
constexpr auto kIsGlibc = false;
#endif

#if defined(__GLIBCXX__)
constexpr auto kIsGlibcxx = true;
#else
constexpr auto kIsGlibcxx = false;
#endif

#if defined(__GLIBCXX__) && _GLIBCXX_RELEASE // major version, 7+
constexpr auto kGlibcxxVer = _GLIBCXX_RELEASE;
#else
constexpr auto kGlibcxxVer = 0;
#endif

#if defined(__GLIBCXX__) && defined(_GLIBCXX_ASSERTIONS)
constexpr auto kGlibcxxAssertions = true;
#else
constexpr auto kGlibcxxAssertions = false;
#endif

#ifdef _LIBCPP_VERSION
constexpr auto kIsLibcpp = true;
#else
constexpr auto kIsLibcpp = false;
#endif

#if defined(__GLIBCXX__)
constexpr auto kIsLibstdcpp = true;
#else
constexpr auto kIsLibstdcpp = false;
#endif

#ifdef _MSC_VER
constexpr auto kMscVer = _MSC_VER;
#else
constexpr auto kMscVer = 0;
#endif

#if defined(__GNUC__) && __GNUC__
constexpr auto kGnuc = __GNUC__;
#else
constexpr auto kGnuc = 0;
#endif

#if __clang__
constexpr auto kIsClang = true;
constexpr auto kClangVerMajor = __clang_major__;
#else
constexpr auto kIsClang = false;
constexpr auto kClangVerMajor = 0;
#endif

#ifdef FOLLY_MICROSOFT_ABI_VER
constexpr auto kMicrosoftAbiVer = FOLLY_MICROSOFT_ABI_VER;
#else
constexpr auto kMicrosoftAbiVer = 0;
#endif

// cpplib is an implementation of the standard library, and is the one typically
// used with the msvc compiler
#ifdef _CPPLIB_VER
constexpr auto kCpplibVer = _CPPLIB_VER;
#else
constexpr auto kCpplibVer = 0;
#endif
} // namespace folly

#define FOLLY_PRAGMA_DETAIL_STR(X) #X

#if defined(_MSC_VER)
#define FOLLY_PRAGMA_UNROLL_N(N)
#elif defined(__GNUC__)
#define FOLLY_PRAGMA_UNROLL_N(N) _Pragma(FOLLY_PRAGMA_DETAIL_STR(GCC unroll(N)))
#else
#define FOLLY_PRAGMA_UNROLL_N(N) _Pragma(FOLLY_PRAGMA_DETAIL_STR(unroll(N)))
#endif

//  MSVC does not permit:
//
//    extern int const num;
//    constexpr int const num = 3;
//
//  Instead:
//
//    extern int const num;
//    FOLLY_STORAGE_CONSTEXPR int const num = 3;
//
//  True as of MSVC 2017.
#ifdef _MSC_VER
#define FOLLY_STORAGE_CONSTEXPR
#else
#define FOLLY_STORAGE_CONSTEXPR constexpr
#endif

//  FOLLY_CXX23_CONSTEXPR
//
//  C++23 permits more cases to be marked constexpr, including definitions of
//  variables of non-literal type in constexpr function as long as they are not
//  constant-evaluated.
#if FOLLY_CPLUSPLUS >= 202302L
#define FOLLY_CXX23_CONSTEXPR constexpr
#else
#define FOLLY_CXX23_CONSTEXPR
#endif

#if defined(FOLLY_CFG_NO_COROUTINES)
#define FOLLY_HAS_COROUTINES 0
#define FOLLY_HAS_IMMOVABLE_COROUTINES 0
#else
#if defined(__NVCC__)
// For now, NVCC matches other compilers but does not offer coroutines.
#define FOLLY_HAS_COROUTINES 0
#elif defined(_WIN32) && defined(__clang__) && !defined(LLVM_COROUTINES) && \
    !defined(LLVM_COROUTINES_CPP20)
// LLVM and MSVC coroutines are ABI incompatible, so for the MSVC implementation
// of <coroutine> on Windows we *don't* have coroutines.
//
// LLVM_COROUTINES indicates that LLVM compatible header is added to include
// path and can be used.
//
// LLVM_COROUTINES_CPP20 indicates that an LLVM compatible header using
// <coroutine> is added to the include path and can be used.

#define FOLLY_HAS_COROUTINES 0
#elif defined(__cpp_impl_coroutine) && __cpp_impl_coroutine >= 201902L && \
    __has_include(<coroutine>)
#define FOLLY_HAS_COROUTINES 1
// FOLLY_NOINLINE here works around an LTO miscompile that promoted
// await_suspend's stack locals onto the coroutine frame (D15143193).
//
// Clang >= 19 fixes the root cause: await_suspend is emitted into an isolated
// wrapper (generateAwaitSuspendWrapper, via the llvm.coro.await.suspend.*
// intrinsics) that the coroutine transform treats as opaque, so its locals
// can't leak onto the frame. There the workaround is redundant for the
// awaiters using this macro (final-suspend / scheduling paths that do not
// capture a return address), so it expands to nothing.
//
// The forward awaiter (Task<T>::Awaiter::await_suspend) uses a separate,
// unconditional FOLLY_NOINLINE for async-stack correctness
// (__builtin_return_address); do not route it through this macro.
#if defined(__clang__) && __clang_major__ >= 19
#define FOLLY_CORO_AWAIT_SUSPEND_NONTRIVIAL_ATTRIBUTES
#else
#define FOLLY_CORO_AWAIT_SUSPEND_NONTRIVIAL_ATTRIBUTES FOLLY_NOINLINE
#endif
#else
#define FOLLY_HAS_COROUTINES 0
#endif

#if !FOLLY_HAS_COROUTINES
#define FOLLY_HAS_IMMOVABLE_COROUTINES 0
// This logic is written as "good until proven broken" because it's possible
// that there's a good compiler older than the oldest good version I checked.
#elif defined(__clang_major__) && __clang_major__ <= 14
//  - 12.0.1 is bad: https://godbolt.org/z/6s489xE8P
//  - 14 is still bad: https://godbolt.org/z/nW1W8cWvb
//  - 15.0.0 is good: https://godbolt.org/z/Tco4c9hbq and sEaKKTf8r
#define FOLLY_HAS_IMMOVABLE_COROUTINES 0
// On Windows, Clang with -fms-compatibility defines _MSC_FULL_VER to
// emulate MSVC - even for newer versions of Clang.
// Explicitly allow Clang 15+, prior to checking _MSC_FULL_VER
#elif defined(__clang_major__) && __clang_major__ >= 15
#define FOLLY_HAS_IMMOVABLE_COROUTINES 1
// BEWARE: Older versions of Clang pretend to be MSVC and define
// `_MSC_FULL_VER`
#elif defined(_MSC_FULL_VER) && _MSC_FULL_VER <= 192930040
//  - 192930040 is bad: https://godbolt.org/z/E797W8xTT
//  - 192930153 is good: https://godbolt.org/z/cM4nW5rTK
#define FOLLY_HAS_IMMOVABLE_COROUTINES 0
#else
#define FOLLY_HAS_IMMOVABLE_COROUTINES 1 // good until proven broken
#endif
#endif // FOLLY_CFG_NO_COROUTINES

#if FOLLY_HAS_COROUTINES
#define FOLLY_HAS_RESULT 1
#else
#define FOLLY_HAS_RESULT 0
#endif
