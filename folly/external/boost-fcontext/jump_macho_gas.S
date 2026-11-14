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

// Selects the Mach-O variant for the target architecture, so that each slice
// of a universal binary gets its own.

#if defined(__aarch64__) || defined(__arm64__)
#include "jump_arm64_aapcs_macho_gas.S"
#elif defined(__x86_64__)
#include "jump_x86_64_sysv_macho_gas.S"
#else
#error "Unsupported architecture"
#endif
