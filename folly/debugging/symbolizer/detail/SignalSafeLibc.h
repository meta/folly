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

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <atomic>
#include <cerrno>
#include <cstddef>

#include <folly/CPortability.h>
#include <folly/lang/Hint.h>

#include <folly/portability/Config.h>
#include <folly/portability/Fcntl.h>
#include <folly/portability/SysMman.h>
#include <folly/portability/Unistd.h>

#if FOLLY_HAVE_SWAPCONTEXT
#ifdef __APPLE__
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#endif

namespace folly {
namespace symbolizer {
namespace detail {
namespace signal_safe_libc {

/// Warm ordinary libc calls and resolve context-switch bindings before
/// installing handlers: binding resolution is not async-signal-safe.
bool initialize() noexcept;

#ifndef _WIN32

namespace impl {

struct Functions {
#if FOLLY_HAVE_SWAPCONTEXT
  int (*getcontext)(ucontext_t*){};
  void (*makecontext)(ucontext_t*, void (*)(), int, ...){};
  int (*swapcontext)(const ucontext_t*, ucontext_t*){};
#endif

  explicit operator bool() const noexcept {
#if FOLLY_HAVE_SWAPCONTEXT
    return getcontext && makecontext && swapcontext;
#else
    return false;
#endif
  }
};

using Resolver = void* (*)(const char*);
Functions loadFunctions(Resolver resolver) noexcept;

extern std::atomic<const Functions*> published;
static_assert(std::atomic<const Functions*>::is_always_lock_free);

inline const Functions* get() noexcept {
  return published.load(std::memory_order_acquire);
}

} // namespace impl

int open(const char* path, int flags) noexcept;
#if FOLLY_HAVE_ELF
int fstat(int fd, struct stat* stat) noexcept;
#endif
off_t lseek(int fd, off_t offset, int whence) noexcept;
ssize_t pread(int fd, void* buf, size_t count, off_t offset) noexcept;
ssize_t read(int fd, void* buf, size_t count) noexcept;
int close(int fd) noexcept;
void* mmap(
    void* addr,
    size_t length,
    int prot,
    int flags,
    int fd,
    off_t offset) noexcept;
int munmap(void* addr, size_t length) noexcept;
void* memcpy(void* dest, const void* src, size_t count) noexcept;
size_t strlcpy(char* dest, const char* src, size_t size) noexcept;
const char* strrchr(const char* str, int ch) noexcept;
int memcmp(const void* lhs, const void* rhs, size_t count) noexcept;
ssize_t readlink(const char* path, char* buf, size_t size) noexcept;
int fill_signal_set(sigset_t* set) noexcept;
int set_signal_mask(int how, const sigset_t* set, sigset_t* oldset) noexcept;

/// Inline startup probes into each caller to resolve its wrapper bindings too.
/// Invalid-FD/path probes may still issue failing syscalls; interposers may
/// return without forwarding, so error-path warmup cannot guarantee coverage.
FOLLY_ALWAYS_INLINE void warmUp() noexcept {
  const int savedErrno = errno;
  int fd = -1;
  compiler_must_not_predict(fd);
  char byte{};
  const auto opened = signal_safe_libc::open("", O_RDONLY | O_CLOEXEC);
#if FOLLY_HAVE_ELF
  struct stat stat{};
  (void)signal_safe_libc::fstat(fd, &stat);
#endif
  (void)signal_safe_libc::lseek(fd, 0, SEEK_SET);
  (void)signal_safe_libc::pread(fd, &byte, 1, 0);
  (void)signal_safe_libc::read(fd, &byte, 1);
  (void)signal_safe_libc::close(opened);
  (void)signal_safe_libc::readlink("", &byte, 1);
  (void)signal_safe_libc::mmap(
      nullptr, 0, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, fd, 0);
  (void)signal_safe_libc::munmap(nullptr, 0);

  const char* left = "a";
  const char* right = "b";
  compiler_must_not_predict(left);
  compiler_must_not_predict(right);
  const auto compared = signal_safe_libc::memcmp(left, right, 1);
  const auto found = signal_safe_libc::strrchr(left, 'a');
  compiler_must_not_elide(compared);
  compiler_must_not_elide(found);
  (void)signal_safe_libc::memcpy(&byte, left, 1);
  compiler_must_not_elide(byte);
  char text[2];
  const auto copied = signal_safe_libc::strlcpy(text, left, sizeof(text));
  compiler_must_not_elide(copied);
  compiler_must_not_elide(text);
  sigset_t set{};
  (void)signal_safe_libc::fill_signal_set(&set);
  compiler_must_not_elide(set);
  (void)signal_safe_libc::set_signal_mask(SIG_SETMASK, nullptr, &set);
  errno = savedErrno;
}

#endif

} // namespace signal_safe_libc
} // namespace detail
} // namespace symbolizer
} // namespace folly
