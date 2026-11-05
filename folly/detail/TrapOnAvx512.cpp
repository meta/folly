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

#include <folly/detail/TrapOnAvx512.h>

#include <folly/Portability.h>

#include <cstdint>
#include <cstring>

namespace folly::detail {
#if FOLLY_X64 && defined(__AVX512F__)
namespace {
void detectTrapOnAvx512Helper() {
  __asm__ volatile("vscalefpd %zmm2, %zmm17, %zmm19");
}
} // namespace

bool hasTrapOnAvx512() {
  return hasTrapOnEntry((void*)detectTrapOnAvx512Helper);
}
#else
bool hasTrapOnAvx512() {
  return false;
}
#endif

bool hasTrapOnEntry([[maybe_unused]] const void* fn) {
#if FOLLY_X64
  static constexpr uint8_t kUd2[] = {0x0f, 0x0b};
  return memcmp(fn, kUd2, sizeof(kUd2)) == 0;
#else
  return false;
#endif
}
} // namespace folly::detail
