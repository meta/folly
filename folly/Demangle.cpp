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

#include <folly/Demangle.h>

#include <algorithm>
#include <cstring>
#include <functional>

#include <folly/CPortability.h>
#include <folly/Utility.h>
#include <folly/functional/Invoke.h>
#include <folly/lang/CString.h>

#if __has_include(<cxxabi.h>)
#include <cxxabi.h> // @donotremove
#endif

//  The headers <libiberty.h> (binutils) and <string.h> (glibc) both declare the
//  symbol basename. Unfortunately, the declarations are different. So including
//  both headers in the same translation unit fails due to the two conflicting
//  declarations. Since <demangle.h> includes <libiberty.h> we must be careful.
#if __has_include(<demangle.h>)
#pragma push_macro("HAVE_DECL_BASENAME")
#define HAVE_DECL_BASENAME 1
#include <demangle.h> // @manual
#pragma pop_macro("HAVE_DECL_BASENAME")
#endif

//  try to find cxxabi demangle
//
//  prefer using a weakref

#if __has_include(<cxxabi.h>)

[[gnu::weakref("__cxa_demangle")]] static char* cxxabi_demangle(
    char const*, char*, size_t*, int*);

#else // __has_include(<cxxabi.h>)

static constexpr auto cxxabi_demangle = static_cast<char* (*)(...)>(nullptr);

#endif // __has_include(<cxxabi.h>)

//  try to find liberty demangle
//
//  cannot use a weak symbol since it may be the only referenced symbol in
//  liberty
//
//  in contrast with cxxabi, where there are certainly other referenced symbols
//
//  for rust_demangle_callback, detect its declaration in the header

#if __has_include(<demangle.h>)

namespace {
struct poison {};

FOLLY_PUSH_WARNING
FOLLY_GCC_DISABLE_WARNING("-Wunused-function")
[[maybe_unused]] void rust_demangle_callback(poison);
FOLLY_POP_WARNING

[[maybe_unused]] FOLLY_ERASE int rust_demangle_callback_fallback(
    const char*, int, demangle_callbackref, void*) {
  return 0;
}

FOLLY_CREATE_QUAL_INVOKER(
    invoke_rust_demangle_primary, ::rust_demangle_callback);
FOLLY_CREATE_QUAL_INVOKER(
    invoke_rust_demangle_fallback, rust_demangle_callback_fallback);

using invoke_rust_demangle_fn = folly::invoke_first_match<
    invoke_rust_demangle_primary,
    invoke_rust_demangle_fallback>;
constexpr invoke_rust_demangle_fn invoke_rust_demangle;

int call_rust_demangle_callback(
    const char* mangled, int options, demangle_callbackref cb, void* opaque) {
  return invoke_rust_demangle(mangled, options, cb, opaque);
}

} // namespace

using liberty_demangle_t = int(const char*, int, demangle_callbackref, void*);

static constexpr liberty_demangle_t* liberty_cplus_demangle =
    cplus_demangle_v3_callback;
static constexpr liberty_demangle_t* liberty_rust_demangle =
    call_rust_demangle_callback;

#if defined(DMGL_NO_RECURSE_LIMIT)
static constexpr auto liberty_demangle_options_no_recurse_limit =
    DMGL_NO_RECURSE_LIMIT;
#else
static constexpr auto liberty_demangle_options_no_recurse_limit = 0;
#endif

static constexpr auto liberty_demangle_options = //
    DMGL_PARAMS | DMGL_ANSI | DMGL_TYPES | //
    liberty_demangle_options_no_recurse_limit;

#else // __has_include(<demangle.h>)

using liberty_demangle_t = int(...);

static constexpr liberty_demangle_t* liberty_cplus_demangle = nullptr;
static constexpr liberty_demangle_t* liberty_rust_demangle = nullptr;
static constexpr auto liberty_demangle_options = 0;

#endif // __has_include(<demangle.h>)

//  implementations

namespace folly {

bool demangle_build_has_cxxabi() noexcept {
  return to_bool(cxxabi_demangle);
}
bool demangle_build_has_liberty() noexcept {
  bool vals[] = {
      to_bool(liberty_cplus_demangle),
      to_bool(liberty_rust_demangle),
  };
  return std::all_of(std::begin(vals), std::end(vals), std::identity{});
}

namespace {
void demangleStringCallback(const char* str, size_t size, void* p) {
  fbstring* demangle = static_cast<fbstring*>(p);

  demangle->append(str, size);
}
} // namespace

fbstring demangle(const char* name) {
  if (!name) {
    return fbstring();
  }

  if (demangle_max_symbol_size) {
    // GCC's __cxa_demangle() uses on-stack data structures for the
    // parser state which are linear in the number of components of the
    // symbol. For extremely long symbols, this can cause a stack
    // overflow. We set an arbitrary symbol length limit above which we
    // just return the mangled name.
    size_t mangledLen = strlen(name);
    if (mangledLen > demangle_max_symbol_size) {
      return fbstring(name, mangledLen);
    }
  }

  if (folly::demangle_build_has_liberty()) {
    liberty_demangle_t* funcs[] = {
        liberty_rust_demangle,
        liberty_cplus_demangle,
    };

    for (auto func : funcs) {
      fbstring demangled;

      // Unlike most library functions, this returns 1 on success and 0 on
      // failure
      int success = func(
          name, liberty_demangle_options, demangleStringCallback, &demangled);
      if (success && !demangled.empty()) {
        return demangled;
      }
    }
  }

  if (cxxabi_demangle) {
    int status;
    size_t len = 0;
    // malloc() memory for the demangled type name
    char* demangled = cxxabi_demangle(name, nullptr, &len, &status);
    if (status != 0) {
      return name;
    }
    // len is the length of the buffer (including NUL terminator and maybe
    // other junk)
    return fbstring(
        demangled, strlen(demangled), len, AcquireMallocatedString());
  } else {
    return fbstring(name);
  }
}

namespace {

struct DemangleBuf {
  char* dest;
  size_t remaining;
  size_t total;
};

void demangleBufCallback(const char* str, size_t size, void* p) {
  DemangleBuf* buf = static_cast<DemangleBuf*>(p);
  size_t n = std::min(buf->remaining, size);
  memcpy(buf->dest, str, n);
  buf->dest += n;
  buf->remaining -= n;
  buf->total += size;
}

} // namespace

size_t demangle(const char* name, char* out, size_t outSize) {
  if (demangle_max_symbol_size) {
    size_t mangledLen = strlen(name);
    if (mangledLen > demangle_max_symbol_size) {
      if (outSize) {
        size_t n = std::min(mangledLen, outSize - 1);
        memcpy(out, name, n);
        out[n] = '\0';
      }
      return mangledLen;
    }
  }

  if (folly::demangle_build_has_liberty()) {
    liberty_demangle_t* funcs[] = {
        liberty_rust_demangle,
        liberty_cplus_demangle,
    };

    for (auto func : funcs) {
      DemangleBuf dbuf;
      dbuf.dest = out;
      dbuf.remaining = outSize ? outSize - 1 : 0; // leave room for null term
      dbuf.total = 0;

      // Unlike most library functions, this returns 1 on success and 0 on
      // failure
      int success =
          func(name, liberty_demangle_options, demangleBufCallback, &dbuf);
      if (success) {
        if (outSize != 0) {
          *dbuf.dest = '\0';
        }
        return dbuf.total;
      }
    }
  }

  // fallback - just return original
  return folly::strlcpy(out, name, outSize);
}

} // namespace folly
