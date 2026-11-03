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

#include <folly/debugging/symbolizer/detail/SignalSafeLibc.h>

#include <cerrno>
#include <cstring>

#include <folly/CPortability.h>
#include <folly/debugging/symbolizer/detail/Debug.h>
#include <folly/lang/CString.h>
#include <folly/lang/Hint.h>

namespace folly {
namespace symbolizer {
namespace detail {
namespace signal_safe_libc {

#ifndef _WIN32

// Keep warmup and crash-time calls at one shared libc binding boundary.
FOLLY_NOINLINE int open(const char* path, int flags) noexcept {
  // @lint-ignore NULLSAFECLANG
  return ::open(path, flags);
}

#if FOLLY_HAVE_ELF
FOLLY_NOINLINE int fstat(int fd, struct stat* stat) noexcept {
  // @lint-ignore NULLSAFECLANG
  return ::fstat(fd, stat);
}
#endif

FOLLY_NOINLINE off_t lseek(int fd, off_t offset, int whence) noexcept {
  return ::lseek(fd, offset, whence);
}

FOLLY_NOINLINE ssize_t
pread(int fd, void* buf, size_t count, off_t offset) noexcept {
  return ::pread(fd, buf, count, offset);
}

FOLLY_NOINLINE ssize_t read(int fd, void* buf, size_t count) noexcept {
  return ::read(fd, buf, count);
}

FOLLY_NOINLINE int close(int fd) noexcept {
  return ::close(fd);
}

FOLLY_NOINLINE void* mmap(
    void* addr,
    size_t length,
    int prot,
    int flags,
    int fd,
    off_t offset) noexcept {
  return ::mmap(addr, length, prot, flags, fd, offset);
}

FOLLY_NOINLINE int munmap(void* addr, size_t length) noexcept {
  return ::munmap(addr, length);
}

FOLLY_NOINLINE void* memcpy(
    void* dest, const void* src, size_t count) noexcept {
  // @lint-ignore NULLSAFECLANG
  return ::memcpy(dest, src, count);
}

FOLLY_NOINLINE size_t
strlcpy(char* dest, const char* src, size_t size) noexcept {
  return folly::strlcpy(dest, src, size);
}

FOLLY_NOINLINE const char* strrchr(const char* str, int ch) noexcept {
  // @lint-ignore NULLSAFECLANG
  return ::strrchr(str, ch);
}

FOLLY_NOINLINE int memcmp(
    const void* lhs, const void* rhs, size_t count) noexcept {
  // @lint-ignore NULLSAFECLANG
  return ::memcmp(lhs, rhs, count);
}

FOLLY_NOINLINE ssize_t
readlink(const char* path, char* buf, size_t size) noexcept {
  // @lint-ignore NULLSAFECLANG
  return ::readlink(path, buf, size);
}

FOLLY_NOINLINE int fill_signal_set(sigset_t* set) noexcept {
  // @lint-ignore NULLSAFECLANG
  return sigfillset(set);
}

FOLLY_NOINLINE int set_signal_mask(
    int how, const sigset_t* set, sigset_t* oldset) noexcept {
  return ::sigprocmask(how, set, oldset);
}

namespace {

template <typename Function>
Function resolve(impl::Resolver resolver, const char* name) noexcept {
  return reinterpret_cast<Function>(resolver(name));
}

} // namespace

namespace impl {

std::atomic<const Functions*> published{nullptr};

Functions loadFunctions(Resolver resolver) noexcept {
  Functions functions{};
#if FOLLY_HAVE_SWAPCONTEXT
  functions.getcontext =
      resolve<decltype(functions.getcontext)>(resolver, "getcontext");
  functions.makecontext =
      resolve<decltype(functions.makecontext)>(resolver, "makecontext");
  functions.swapcontext =
      resolve<decltype(functions.swapcontext)>(resolver, "swapcontext");
#else
  (void)resolver;
#endif
  return functions;
}

} // namespace impl

bool initialize() noexcept {
  static const auto functions = [] {
    warmUp();
    return impl::loadFunctions(get_libc_symbol);
  }();
  if (!functions) {
    return false;
  }
  impl::published.store(&functions, std::memory_order_release);
  return true;
}

#else

bool initialize() noexcept {
  return false;
}

#endif

} // namespace signal_safe_libc
} // namespace detail
} // namespace symbolizer
} // namespace folly
