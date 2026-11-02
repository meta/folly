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

#include <folly/debugging/symbolizer/detail/Debug.h>

#include <folly/Portability.h>
#include <folly/system/libc.h>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#if defined(__APPLE__) && !TARGET_OS_OSX
#define FOLLY_DETAIL_HAS_R_DEBUG 0
#elif !defined(__linux__) || !FOLLY_HAVE_ELF || !FOLLY_HAVE_DWARF
#define FOLLY_DETAIL_HAS_R_DEBUG 0
#else
#define FOLLY_DETAIL_HAS_R_DEBUG 1
#endif

namespace folly {
namespace symbolizer {
namespace detail {

namespace {

void* get_libc_handle() {
#ifndef _WIN32
  static void* const handle = dlopen(
      folly::detail::libc_soname(), RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
  return handle;
#else
  return nullptr;
#endif
}

} // namespace

#if FOLLY_DETAIL_HAS_R_DEBUG

/// There is a strong requirement of finding the true _r_debug in ld.so.
///
/// The previous code used the declaration of the extern variable as found in
///
///   #include <link.h>
///
/// The intention of using the extern variable is to get the toolchain to emit a
/// GOT access, e.g. like:
///
///   mov rax, qword ptr [rip + _r_debug@GOTPCREL]
///
/// This works in typical conditions. However, in the case of compiling with:
///
///   -mcmodel=small -fno-pic
///
/// ie with non-pic and small-code-model, there is a different outcome. Here
/// instead, the toolchain emits a COPY relocation to copy _r_debug into the
/// executable and emits a direct non-GOT access to the copy like:
///
///   mov eax, offset _r_debug
///
/// This causes all accesses in ld.so to be redirected to the copy in the main
/// executable, which is a valid approach.
///
/// However, LLDB specifically looks for _r_debug in ld.so. When the debugger
/// inspects a process, the _r_debug in the executable has all of the current
/// information about the link and load state, but the debugger looks only at
/// the _r_debug in ld.so which is empty! This breaks debugging.
static r_debug* r_debug_cache_;
[[gnu::constructor(101)]] void r_debug_cache_init_() {
  r_debug_cache_ = static_cast<r_debug*>(dlsym(RTLD_DEFAULT, "_r_debug"));
}

#endif

struct r_debug* get_r_debug() {
#if FOLLY_DETAIL_HAS_R_DEBUG
  return r_debug_cache_;
#else
  return nullptr;
#endif
}

void* get_libc_symbol(const char* name) {
  if (!name) {
    return nullptr;
  }
#ifndef _WIN32
  auto* handle = get_libc_handle();
  return handle ? dlsym(handle, name) : nullptr;
#else
  return nullptr;
#endif
}

} // namespace detail
} // namespace symbolizer
} // namespace folly
