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

// On Emscripten/musl, the `gettid()` declaration in <unistd.h> is gated on
// _GNU_SOURCE. Define it before any header is included so the declaration is
// visible to the `__EMSCRIPTEN__` branch in `getOSThreadIDSlow()` below.
// Scoped to `__EMSCRIPTEN__` to avoid cascading GNU extensions into the file
// on other platforms (Linux uses `syscall(FOLLY_SYS_gettid)` and does not
// need _GNU_SOURCE here).
#if defined(__EMSCRIPTEN__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <folly/system/ThreadId.h>

#include <folly/Likely.h>
#include <folly/portability/PThread.h>
#include <folly/portability/SysSyscall.h>
#include <folly/portability/Unistd.h>
#include <folly/portability/Windows.h>
#include <folly/synchronization/RelaxedAtomic.h>
#include <folly/system/AtFork.h>

namespace folly {

uint64_t detail::get_unique_thread_id_slow() {
  static relaxed_atomic<uint64_t> counter{0};
  static thread_local uint64_t id{0};
  return FOLLY_LIKELY(id != 0) ? id : (id = ++counter);
}

uint64_t getCurrentThreadID() {
#if defined(__APPLE__)
  return uint64_t(pthread_mach_thread_np(pthread_self()));
#elif defined(_WIN32)
  return uint64_t(GetCurrentThreadId());
#else
  return uint64_t(pthread_self());
#endif
}

namespace detail {

uint64_t getOSThreadIDSlow() {
#if defined(__APPLE__)
  uint64_t tid;
  pthread_threadid_np(nullptr, &tid);
  return tid;
#elif defined(_WIN32)
  return uint64_t(GetCurrentThreadId());
#elif defined(__FreeBSD__)
  long tid;
  thr_self(&tid);
  return uint64_t(tid);
#elif defined(__EMSCRIPTEN__)
  return uint64_t(gettid());
#elif defined(__wasi__)
  // wasi has no syscalls and no kernel thread ids. This is pthread_self(), the
  // same value getCurrentThreadID() returns: nonzero and distinct among live
  // threads, but reusable after a thread exits.
  return uint64_t(pthread_self());
#else
  return uint64_t(syscall(FOLLY_SYS_gettid));
#endif
}

} // namespace detail

namespace {

struct CacheState {
  CacheState() {
    AtFork::registerHandler(this, [] { return true; }, [] {}, [] { ++epoch; });
  }
  ~CacheState() { AtFork::unregisterHandler(this); }

  // Used to invalidate all caches in the child process on fork. Start at 1 so
  // that 0 is always invalid.
  static relaxed_atomic<uint64_t> epoch;
};

relaxed_atomic<uint64_t> CacheState::epoch{1};

CacheState gCacheState;

} // namespace

uint64_t getOSThreadID() {
  thread_local std::pair<uint64_t, uint64_t> cache{0, 0};
  auto epoch = CacheState::epoch.load();
  if (epoch != cache.first) [[unlikely]] {
    cache = {epoch, detail::getOSThreadIDSlow()};
  }
  return cache.second;
}

} // namespace folly
