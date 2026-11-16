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

#include <folly/memory/SanitizeAddress.h>

#include <folly/lang/Extern.h>

//  Address Sanitizer interface may be found at:
//    https://github.com/llvm/llvm-project/blob/main/compiler-rt/include/sanitizer/asan_interface.h
//    https://github.com/llvm/llvm-project/blob/main/compiler-rt/include/sanitizer/common_interface_defs.h
extern "C" void __asan_poison_memory_region(void const volatile*, std::size_t);
extern "C" void __asan_unpoison_memory_region(
    void const volatile*, std::size_t);
extern "C" void* __asan_region_is_poisoned(void*, std::size_t);
extern "C" int __asan_address_is_poisoned(void const volatile*);
extern "C" void __sanitizer_start_switch_fiber(
    void**, void const*, std::size_t);
extern "C" void __sanitizer_finish_switch_fiber(
    void*, void const**, std::size_t*);

namespace {

FOLLY_CREATE_EXTERN_ACCESSOR(
    asan_poison_memory_region_access_v, __asan_poison_memory_region);
FOLLY_CREATE_EXTERN_ACCESSOR(
    asan_unpoison_memory_region_access_v, __asan_unpoison_memory_region);
FOLLY_CREATE_EXTERN_ACCESSOR(
    asan_region_is_poisoned_access_v, __asan_region_is_poisoned);
FOLLY_CREATE_EXTERN_ACCESSOR(
    asan_address_is_poisoned_access_v, __asan_address_is_poisoned);
FOLLY_CREATE_EXTERN_ACCESSOR(
    sanitizer_start_switch_fiber_access_v, __sanitizer_start_switch_fiber);
FOLLY_CREATE_EXTERN_ACCESSOR(
    sanitizer_finish_switch_fiber_access_v, __sanitizer_finish_switch_fiber);

constexpr bool EAsan = folly::kIsLibrarySanitizeAddress;
constexpr bool EFiber = EAsan || folly::kIsLibrarySanitizeHwAddress;

} // namespace

namespace folly {
namespace detail {

constinit asan_mark_memory_region_t* const asan_poison_memory_region_v =
    asan_poison_memory_region_access_v<EAsan>;

constinit asan_mark_memory_region_t* const asan_unpoison_memory_region_v =
    asan_unpoison_memory_region_access_v<EAsan>;

constinit asan_region_is_poisoned_t* const asan_region_is_poisoned_v =
    asan_region_is_poisoned_access_v<EAsan>;

constinit asan_address_is_poisoned_t* const asan_address_is_poisoned_v =
    asan_address_is_poisoned_access_v<EAsan>;

constinit sanitizer_start_switch_fiber_t* const sanitizer_start_switch_fiber_v =
    sanitizer_start_switch_fiber_access_v<EFiber>;

constinit sanitizer_finish_switch_fiber_t* const
    sanitizer_finish_switch_fiber_v =
        sanitizer_finish_switch_fiber_access_v<EFiber>;

} // namespace detail
} // namespace folly
