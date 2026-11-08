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

#include <errno.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>

#include <folly/Conv.h>
#include <folly/FBString.h>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/lang/cstring_view.h>
#include <folly/portability/SysTypes.h>

namespace folly {

// Various helpers to throw appropriate std::system_error exceptions from C
// library errors (returned in errno, as positive return values (many POSIX
// functions), or as negative return values (Linux syscalls))
//
// The *Explicit functions take an explicit value for errno.

// On linux and similar platforms the value of `errno` is a mixture of
// POSIX-`errno`-domain error codes and per-OS extended error codes. So the
// most appropriate category to use is `system_category`.
//
// On Windows `system_category` means codes that can be returned by Win32 API
// `GetLastError` and codes from the `errno`-domain must be reported as
// `generic_category`.
inline const std::error_category& errorCategoryForErrnoDomain() noexcept {
  if (kIsWindows) {
    return std::generic_category();
  }
  return std::system_category();
}

inline std::system_error makeSystemErrorExplicit(int err) {
  return std::system_error(err, errorCategoryForErrnoDomain());
}

// The common case. This also disambiguates a const char* / string-literal
// argument, which would otherwise convert equally well to both the cstring_view
// and the std::string overload below (an ambiguous call).
inline std::system_error makeSystemErrorExplicit(int err, const char* msg) {
  return std::system_error(err, errorCategoryForErrnoDomain(), msg);
}

// Any other single NUL-terminated string-like argument (e.g. fbstring,
// cstring_view). It already owns a NUL-terminated buffer, so hand its c_str()
// straight to the ctor instead of paying for a to<fbstring> conversion and
// fbstring unshare.
inline std::system_error makeSystemErrorExplicit(int err, cstring_view msg) {
  return std::system_error(err, errorCategoryForErrnoDomain(), msg.c_str());
}

// std::string is stored directly by std::system_error, so pass it through
// unchanged. This is preferred over the cstring_view overload for std::string
// arguments and, unlike that overload, preserves any embedded NUL bytes.
inline std::system_error makeSystemErrorExplicit(
    int err, const std::string& msg) {
  return std::system_error(err, errorCategoryForErrnoDomain(), msg);
}

// Fallback for callers passing multiple pieces to concatenate, or a single
// non-string argument: build the std::string directly rather than routing
// through fbstring. A single string-like argument is handled by the overloads
// above, so it is excluded here to keep those fast paths (an unconstrained
// template would otherwise be a better match than their conversions).
template <
    class... Args,
    std::enable_if_t<
        !(sizeof...(Args) == 1 &&
          (std::is_convertible_v<Args, cstring_view> && ...)),
        int> = 0>
std::system_error makeSystemErrorExplicit(int err, Args&&... args) {
  return std::system_error(
      err,
      errorCategoryForErrnoDomain(),
      to<std::string>(std::forward<Args>(args)...));
}

inline std::system_error makeSystemError(const char* msg) {
  return makeSystemErrorExplicit(errno, msg);
}

template <class... Args>
std::system_error makeSystemError(Args&&... args) {
  return makeSystemErrorExplicit(errno, std::forward<Args>(args)...);
}

// Helper to throw std::system_error
[[noreturn]] inline void throwSystemErrorExplicit(int err, const char* msg) {
  throw_exception(makeSystemErrorExplicit(err, msg));
}

template <class... Args>
[[noreturn]] void throwSystemErrorExplicit(int err, Args&&... args) {
  throw_exception(makeSystemErrorExplicit(err, std::forward<Args>(args)...));
}

// Helper to throw std::system_error from errno and components of a string
template <class... Args>
[[noreturn]] void throwSystemError(Args&&... args) {
  throwSystemErrorExplicit(errno, std::forward<Args>(args)...);
}

// Check a Posix return code (0 on success, error number on error), throw
// on error.
template <class... Args>
void checkPosixError(int err, Args&&... args) {
  if (err != 0) [[unlikely]] {
    throwSystemErrorExplicit(err, std::forward<Args>(args)...);
  }
}

// Check a Linux kernel-style return code (>= 0 on success, negative error
// number on error), throw on error.
template <class... Args>
void checkKernelError(ssize_t ret, Args&&... args) {
  if (ret < 0) [[unlikely]] {
    throwSystemErrorExplicit(int(-ret), std::forward<Args>(args)...);
  }
}

// Check a traditional Unix return code (-1 and sets errno on error), throw
// on error.
template <class... Args>
void checkUnixError(ssize_t ret, Args&&... args) {
  if (ret == -1) [[unlikely]] {
    throwSystemError(std::forward<Args>(args)...);
  }
}

template <class... Args>
void checkUnixErrorExplicit(ssize_t ret, int savedErrno, Args&&... args) {
  if (ret == -1) [[unlikely]] {
    throwSystemErrorExplicit(savedErrno, std::forward<Args>(args)...);
  }
}

// Check the return code from a fopen-style function (returns a non-nullptr
// FILE* on success, nullptr on error, sets errno).  Works with fopen, fdopen,
// freopen, tmpfile, etc.
template <class... Args>
void checkFopenError(FILE* fp, Args&&... args) {
  if (!fp) [[unlikely]] {
    throwSystemError(std::forward<Args>(args)...);
  }
}

template <class... Args>
void checkFopenErrorExplicit(FILE* fp, int savedErrno, Args&&... args) {
  if (!fp) [[unlikely]] {
    throwSystemErrorExplicit(savedErrno, std::forward<Args>(args)...);
  }
}

/**
 * If cond is not true, raise an exception of type E.  E must have a ctor that
 * works with const char* (a description of the failure).
 */
#define CHECK_THROW(cond, E)                       \
  do {                                             \
    if (!(cond)) {                                 \
      folly::throw_exception<E>(                   \
          "Check failed: " #cond ", in " __FILE__  \
          ":" FOLLY_PP_STRINGIZE_MACRO(__LINE__)); \
    }                                              \
  } while (0)

} // namespace folly
