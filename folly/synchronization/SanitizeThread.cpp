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

#include <folly/synchronization/SanitizeThread.h>

#include <folly/lang/Extern.h>

// abseil uses size_t for size params while other FB code and libraries use
// long, so it is helpful to keep these declarations out of widely-included
// header files.

extern "C" void __tsan_acquire(void* addr);

extern "C" void __tsan_release(void* addr);

extern "C" void AnnotateRWLockCreate(
    char const* f, int l, const volatile void* addr);

extern "C" void AnnotateRWLockCreateStatic(
    char const* f, int l, const volatile void* addr);

extern "C" void AnnotateRWLockDestroy(
    char const* f, int l, const volatile void* addr);

extern "C" void AnnotateRWLockAcquired(
    char const* f, int l, const volatile void* addr, long w);

extern "C" void AnnotateRWLockReleased(
    char const* f, int l, const volatile void* addr, long w);

extern "C" void AnnotateBenignRaceSized(
    char const* f,
    int l,
    const volatile void* addr,
    long size,
    char const* desc);

extern "C" void AnnotateIgnoreReadsBegin(char const* f, int l);

extern "C" void AnnotateIgnoreReadsEnd(char const* f, int l);

extern "C" void AnnotateIgnoreWritesBegin(char const* f, int l);

extern "C" void AnnotateIgnoreWritesEnd(char const* f, int l);

extern "C" void AnnotateIgnoreSyncBegin(char const* f, int l);

extern "C" void AnnotateIgnoreSyncEnd(char const* f, int l);

namespace {

FOLLY_CREATE_EXTERN_ACCESSOR(tsan_acquire_access_v, __tsan_acquire);

FOLLY_CREATE_EXTERN_ACCESSOR(tsan_release_access_v, __tsan_release);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_rwlock_create_access_v, AnnotateRWLockCreate);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_rwlock_create_static_access_v, AnnotateRWLockCreateStatic);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_rwlock_destroy_access_v, AnnotateRWLockDestroy);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_rwlock_acquired_access_v, AnnotateRWLockAcquired);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_rwlock_released_access_v, AnnotateRWLockReleased);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_benign_race_sized_access_v, AnnotateBenignRaceSized);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_ignore_reads_begin_access_v, AnnotateIgnoreReadsBegin);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_ignore_reads_end_access_v, AnnotateIgnoreReadsEnd);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_ignore_writes_begin_access_v, AnnotateIgnoreWritesBegin);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_ignore_writes_end_access_v, AnnotateIgnoreWritesEnd);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_ignore_sync_begin_access_v, AnnotateIgnoreSyncBegin);

FOLLY_CREATE_EXTERN_ACCESSOR(
    annotate_ignore_sync_end_access_v, AnnotateIgnoreSyncEnd);

} // namespace

static constexpr auto const E = folly::kIsSanitizeThread;

namespace folly {
namespace detail {

FOLLY_STORAGE_CONSTEXPR tsan_acquire_t* const tsan_acquire_v =
    tsan_acquire_access_v<E>;

FOLLY_STORAGE_CONSTEXPR tsan_release_t* const tsan_release_v =
    tsan_release_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_rwlock_cd_t* const annotate_rwlock_create_v =
    annotate_rwlock_create_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_rwlock_cd_t* const
    annotate_rwlock_create_static_v = annotate_rwlock_create_static_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_rwlock_cd_t* const annotate_rwlock_destroy_v =
    annotate_rwlock_destroy_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_rwlock_ar_t* const annotate_rwlock_acquired_v =
    annotate_rwlock_acquired_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_rwlock_ar_t* const annotate_rwlock_released_v =
    annotate_rwlock_released_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_benign_race_sized_t* const
    annotate_benign_race_sized_v = annotate_benign_race_sized_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_ignore_t* const annotate_ignore_reads_begin_v =
    annotate_ignore_reads_begin_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_ignore_t* const annotate_ignore_reads_end_v =
    annotate_ignore_reads_end_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_ignore_t* const
    annotate_ignore_writes_begin_v = annotate_ignore_writes_begin_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_ignore_t* const annotate_ignore_writes_end_v =
    annotate_ignore_writes_end_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_ignore_t* const annotate_ignore_sync_begin_v =
    annotate_ignore_sync_begin_access_v<E>;

FOLLY_STORAGE_CONSTEXPR annotate_ignore_t* const annotate_ignore_sync_end_v =
    annotate_ignore_sync_end_access_v<E>;

} // namespace detail
} // namespace folly
