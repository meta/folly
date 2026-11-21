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

#include <chrono>
#include <cstdint>

//  Description of and implementation of sequences for precise measurement:
//    https://github.com/abseil/abseil-cpp/blob/20240116.2/absl/random/internal/nanobenchmark.cc#L164-L277

#if defined(_MSC_VER) && !defined(__clang__) && \
    (defined(_M_IX86) || defined(_M_X64))
extern "C" std::uint64_t __rdtsc();
extern "C" std::uint64_t __rdtscp(unsigned int*);
extern "C" void _ReadWriteBarrier();
extern "C" void _mm_lfence();
#pragma intrinsic(__rdtsc)
#pragma intrinsic(__rdtscp)
#pragma intrinsic(_ReadWriteBarrier)
#pragma intrinsic(_mm_lfence)
#endif

namespace folly {

inline std::uint64_t hardware_timestamp() {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
  return __rdtsc();
#elif defined(__GNUC__) && (defined(__i386__) || FOLLY_X64)
  return __builtin_ia32_rdtsc();
#elif FOLLY_AARCH64 && !FOLLY_MOBILE
  uint64_t cval;
  asm volatile("mrs %0, cntvct_el0" : "=r"(cval));
  return cval;
#else
  // use steady_clock::now() as an approximation for the timestamp counter on
  // non-x86 systems
  return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}

/// hardware_timestamp_measurement_start
/// hardware_timestamp_measurement_stop
///
/// Suitable for beginning precise measurement of a region of code.
///
/// Prevents the compiler from reordering instructions across the call. This is
/// in contrast with hardware_timestamp(), which may not prevent the compiler
/// from reordering instructions across the call.
///
/// Prevents the processor from pipelining the call with loads. This is in
/// contrast with hardware_timestamp(), which allows the processor to pipeline
/// the call with surrounding loads.
///
/// Does not prevent instruction pipelines from continuing execution across the
/// call. For example, does not prevent a store issued before the call from
/// continuing execution (becoming globally visible) across the call. This means
/// that this form may not be suitable for certain measurement use-cases which
/// would require such prevention. However, this would be suitable for typical
/// measurement use-cases which do not specifically need to measure background
/// work such as store execution.
std::uint64_t hardware_timestamp_measurement_start() noexcept;
std::uint64_t hardware_timestamp_measurement_stop() noexcept;

inline std::uint64_t hardware_timestamp_measurement_start() noexcept {
#if defined(_MSC_VER) && !defined(__clang__) && \
    (defined(_M_IX86) || defined(_M_X64))
  // msvc does not have embedded assembly
  _ReadWriteBarrier();
  _mm_lfence();
  _ReadWriteBarrier();
  auto const ret = __rdtsc();
  _ReadWriteBarrier();
  _mm_lfence();
  _ReadWriteBarrier();
  return ret;
#elif defined(__GNUC__) && FOLLY_X64
  uint64_t ret = 0;
  asm volatile inline(
      "lfence\n"
      "rdtsc\n" // loads 64-bit tsc into edx:eax
      "shl $32, %%rdx\n" // prep rdx for combine into rax
      "or %%rdx, %[ret]\n" // combine rdx into rax
      "lfence\n"
      : [ret] "=a"(ret) // bind ret to rax for output
      : // no inputs
      : "rdx", // rdtsc loads into edx:eax
        "cc", // shl clobbers condition-code
        "memory" // memory clobber asks gcc/clang not to reorder
  );
  return ret;
#else
  // use steady_clock::now() as an approximation for the timestamp counter on
  // non-x64 systems
  return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}

inline std::uint64_t hardware_timestamp_measurement_stop() noexcept {
#if defined(_MSC_VER) && !defined(__clang__) && \
    (defined(_M_IX86) || defined(_M_X64))
  // msvc does not have embedded assembly
  _ReadWriteBarrier();
  unsigned int aux;
  auto const ret = __rdtscp(&aux);
  _ReadWriteBarrier();
  _mm_lfence();
  _ReadWriteBarrier();
  return ret;
#elif defined(__GNUC__) && FOLLY_X64
  uint64_t ret = 0;
  asm volatile inline(
      "rdtscp\n" // loads 64-bit tsc into edx:eax, clobbers ecx
      "shl $32, %%rdx\n" // prep rdx for combine into rax
      "or %%rdx, %[ret]\n" // combine rdx into rax
      "lfence\n"
      : [ret] "=a"(ret) // bind ret to rax for output
      : // no inputs
      : "rdx", // rdtscp loads into edx:eax
        "rcx", // rdtscp clobbers rcx
        "cc", // shl clobbers condition-code
        "memory" // memory clobber asks gcc/clang not to reorder
  );
  return ret;
#else
  // use steady_clock::now() as an approximation for the timestamp counter on
  // non-x64 systems
  return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
}

} // namespace folly
