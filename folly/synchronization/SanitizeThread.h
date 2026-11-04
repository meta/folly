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

#include <folly/Portability.h>

namespace folly {

enum class annotate_rwlock_level : long {
  rdlock = 0,
  wrlock = 1,
};

namespace detail {

using tsan_acquire_t = void(void*);
using tsan_release_t = void(void*);
using annotate_rwlock_cd_t = void(char const*, int, void const volatile*);
using annotate_rwlock_ar_t = void(char const*, int, void const volatile*, long);
using annotate_benign_race_sized_t =
    void(char const*, int, void const volatile*, long, char const*);
using annotate_ignore_t = void(char const*, int);

extern tsan_acquire_t* const tsan_acquire_v;
extern tsan_release_t* const tsan_release_v;
extern annotate_rwlock_cd_t* const annotate_rwlock_create_v;
extern annotate_rwlock_cd_t* const annotate_rwlock_create_static_v;
extern annotate_rwlock_cd_t* const annotate_rwlock_destroy_v;
extern annotate_rwlock_ar_t* const annotate_rwlock_acquired_v;
extern annotate_rwlock_ar_t* const annotate_rwlock_released_v;
extern annotate_benign_race_sized_t* const annotate_benign_race_sized_v;
extern annotate_ignore_t* const annotate_ignore_reads_begin_v;
extern annotate_ignore_t* const annotate_ignore_reads_end_v;
extern annotate_ignore_t* const annotate_ignore_writes_begin_v;
extern annotate_ignore_t* const annotate_ignore_writes_end_v;
extern annotate_ignore_t* const annotate_ignore_sync_begin_v;
extern annotate_ignore_t* const annotate_ignore_sync_end_v;

} // namespace detail

//  tsan_acquire
//
//  Establishes a happens-before relation with a preceding tsan_release on the
//  same address.
//
//  mimic: __tsan_acquire, llvm compiler-rt
FOLLY_ALWAYS_INLINE static void tsan_acquire(void* const addr) {
  auto fun = detail::tsan_acquire_v;
  return kIsSanitizeThread && fun ? fun(addr) : void();
}

//  tsan_release
//
//  Establishes a happens-before relation with a subsequent tsan_acquire on the
//  same address.
//
//  mimic: __tsan_release, llvm compiler-rt
FOLLY_ALWAYS_INLINE static void tsan_release(void* const addr) {
  auto fun = detail::tsan_release_v;
  return kIsSanitizeThread && fun ? fun(addr) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_rwlock_create(
    void const volatile* const addr, char const* const f, int const l) {
  auto fun = detail::annotate_rwlock_create_v;
  return kIsSanitizeThread && fun ? fun(f, l, addr) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_rwlock_create_static(
    void const volatile* const addr, char const* const f, int const l) {
  auto fun = detail::annotate_rwlock_create_static_v;
  return kIsSanitizeThread && fun ? fun(f, l, addr) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_rwlock_destroy(
    void const volatile* const addr, char const* const f, int const l) {
  auto fun = detail::annotate_rwlock_destroy_v;
  return kIsSanitizeThread && fun ? fun(f, l, addr) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_rwlock_acquired(
    void const volatile* const addr,
    annotate_rwlock_level const w,
    char const* const f,
    int const l) {
  auto fun = detail::annotate_rwlock_acquired_v;
  return kIsSanitizeThread && fun ? fun(f, l, addr, long(w)) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_rwlock_try_acquired(
    void const volatile* const addr,
    annotate_rwlock_level const w,
    bool const result,
    char const* const f,
    int const l) {
  return result ? annotate_rwlock_acquired(addr, w, f, l) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_rwlock_released(
    void const volatile* const addr,
    annotate_rwlock_level const w,
    char const* const f,
    int const l) {
  auto fun = detail::annotate_rwlock_released_v;
  return kIsSanitizeThread && fun ? fun(f, l, addr, long(w)) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_benign_race_sized(
    void const volatile* const addr,
    long const size,
    char const* const desc,
    char const* const f,
    int const l) {
  auto fun = detail::annotate_benign_race_sized_v;
  return kIsSanitizeThread && fun ? fun(f, l, addr, size, desc) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_ignore_reads_begin(
    char const* const f, int const l) {
  auto fun = detail::annotate_ignore_reads_begin_v;
  return kIsSanitizeThread && fun ? fun(f, l) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_ignore_reads_end(
    char const* const f, int const l) {
  auto fun = detail::annotate_ignore_reads_end_v;
  return kIsSanitizeThread && fun ? fun(f, l) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_ignore_writes_begin(
    char const* const f, int const l) {
  auto fun = detail::annotate_ignore_writes_begin_v;
  return kIsSanitizeThread && fun ? fun(f, l) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_ignore_writes_end(
    char const* const f, int const l) {
  auto fun = detail::annotate_ignore_writes_end_v;
  return kIsSanitizeThread && fun ? fun(f, l) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_ignore_sync_begin(
    char const* const f, int const l) {
  auto fun = detail::annotate_ignore_sync_begin_v;
  return kIsSanitizeThread && fun ? fun(f, l) : void();
}

FOLLY_ALWAYS_INLINE static void annotate_ignore_sync_end(
    char const* const f, int const l) {
  auto fun = detail::annotate_ignore_sync_end_v;
  return kIsSanitizeThread && fun ? fun(f, l) : void();
}

class annotate_ignore_thread_sanitizer_guard {
 public:
  annotate_ignore_thread_sanitizer_guard(
      char const* const file, int const line) noexcept
      : file_{file}, line_{line} {
    annotate_ignore_reads_begin(file_, line_);
    annotate_ignore_writes_begin(file_, line_);
  }

  annotate_ignore_thread_sanitizer_guard(
      const annotate_ignore_thread_sanitizer_guard&) = delete;
  annotate_ignore_thread_sanitizer_guard& operator=(
      const annotate_ignore_thread_sanitizer_guard&) = delete;

  ~annotate_ignore_thread_sanitizer_guard() {
    annotate_ignore_reads_end(file_, line_);
    annotate_ignore_writes_end(file_, line_);
  }

 private:
  char const* const file_;
  int const line_;
};

} // namespace folly
