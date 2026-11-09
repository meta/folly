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

namespace detail {

using lsan_ignore_object_t = void(void const*);
using lsan_register_root_region_t = void(void const*, std::size_t);
using lsan_unregister_root_region_t = void(void const*, std::size_t);

extern lsan_ignore_object_t* const lsan_ignore_object_v;
extern lsan_register_root_region_t* const lsan_register_root_region_v;
extern lsan_unregister_root_region_t* const lsan_unregister_root_region_v;

void annotate_object_leaked_impl(void const* ptr);
void annotate_object_collected_impl(void const* ptr);
size_t annotate_object_count_leaked_uncollected_impl();

} // namespace detail

//  lsan_ignore_object
//
//  Marks an allocation to be treated as a root when Leak Sanitizer scans for
//  leaked allocations.
[[maybe_unused]] FOLLY_ALWAYS_INLINE static void lsan_ignore_object(
    void const* const ptr) {
  auto fun = detail::lsan_ignore_object_v;
  return kIsSanitizeAddress && fun ? fun(ptr) : void();
}

//  lsan_register_root_region
//
//  Marks a region as a root for Leak Sanitizer scans.
[[maybe_unused]] FOLLY_ALWAYS_INLINE static void lsan_register_root_region(
    void const* const ptr, std::size_t const size) {
  auto fun = detail::lsan_register_root_region_v;
  return kIsSanitizeAddress && fun ? fun(ptr, size) : void();
}

//  lsan_unregister_root_region
//
//  Marks a region as a root for Leak Sanitizer scans.
[[maybe_unused]] FOLLY_ALWAYS_INLINE static void lsan_unregister_root_region(
    void const* const ptr, std::size_t const size) {
  auto fun = detail::lsan_unregister_root_region_v;
  return kIsSanitizeAddress && fun ? fun(ptr, size) : void();
}

/**
 * When the current compilation unit is being compiled with ASAN enabled this
 * function will suppress an intentionally leaked pointer from the LSAN report.
 * Otherwise, this function is an inlinable no-op.
 *
 * NOTE: This function will suppress LSAN leak reporting when the current
 * compilation unit is being compiled with ASAN, independent of whether folly
 * itself was compiled with ASAN enabled.
 */
[[maybe_unused]] FOLLY_ALWAYS_INLINE static void annotate_object_leaked(
    void const* ptr) {
  if (kIsSanitizeAddress) {
    detail::annotate_object_leaked_impl(ptr);
  }
}

/**
 * Annotate that an object previously passed to annotate_object_leaked() has
 * been collected, so we should no longer suppress it from the LSAN report.
 * This function is an inlinable no-op when ASAN is not enabled for the current
 * compilation unit.
 */
[[maybe_unused]] FOLLY_ALWAYS_INLINE static void annotate_object_collected(
    void const* ptr) {
  if (kIsSanitizeAddress) {
    detail::annotate_object_collected_impl(ptr);
  }
}

/**
 * Count how many times objects have been passed to annotate_object_leaked() but
 * not yet passed to annotate_object_collected().
 */
[[maybe_unused]] FOLLY_ALWAYS_INLINE static size_t
annotate_object_count_leaked_uncollected() {
  if (kIsSanitizeAddress) {
    return detail::annotate_object_count_leaked_uncollected_impl();
  }
  return 0;
}

} // namespace folly
