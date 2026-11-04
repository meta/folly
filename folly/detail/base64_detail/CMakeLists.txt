# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# NOTE: This file is manually maintained, not auto-generated.
# Update this file when the corresponding BUCK file changes.

folly_add_library(
  NAME base64_api
  SRCS Base64Api.cpp
  HEADERS Base64Api.h
  DEPS
    folly_cpu_id
    folly_detail_base64_detail_base64_sse4_2
    folly_detail_base64_detail_base64_swar
  EXPORTED_DEPS
    folly_detail_base64_detail_base64_common
    folly_detail_base64_detail_base64_scalar
    folly_portability
)

folly_add_library(
  NAME base64_common
  HEADERS Base64Common.h
  EXPORTED_DEPS
    folly_detail_base64_detail_base64_constants
    folly_utility
)

folly_add_library(
  NAME base64_constants
  HEADERS Base64Constants.h
)

folly_add_library(
  NAME base64_hidden_constants
  HEADERS Base64HiddenConstants.h
  EXPORTED_DEPS folly_detail_base64_detail_base64_constants
)

folly_add_library(
  NAME base64_scalar
  HEADERS Base64Scalar.h
  EXPORTED_DEPS
    folly_detail_base64_detail_base64_common
    folly_detail_base64_detail_base64_constants
    folly_memory_uninitialized_memory_hacks
    folly_utility
)

folly_add_library(
  NAME base64_simd
  HEADERS Base64Simd.h
  EXPORTED_DEPS
    folly_c_portability
    folly_detail_base64_detail_base64_common
    folly_detail_base64_detail_base64_constants
    folly_detail_base64_detail_base64_hidden_constants
    folly_detail_base64_detail_base64_scalar
    folly_detail_base64_detail_base64_swar
)

folly_add_library(
  NAME base64_sse4_2_platform
  HEADERS Base64_SSE4_2_Platform.h
  EXPORTED_DEPS
    folly_detail_base64_detail_base64_hidden_constants
    folly_portability
)

folly_add_library(
  NAME base64_sse4_2
  SRCS Base64_SSE4_2.cpp
  HEADERS Base64_SSE4_2.h
  DEPS
    folly_detail_base64_detail_base64_simd
    folly_detail_base64_detail_base64_sse4_2_platform
  EXPORTED_DEPS
    folly_detail_base64_detail_base64_common
    folly_portability
)

# Apply -msse4.2 flag on x86 (MSVC doesn't need it)
if (IS_X86_64_ARCH AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_options(
    folly_detail_base64_detail_base64_sse4_2_obj
    PRIVATE -msse4.2
  )
endif ()

folly_add_library(
  NAME base64_swar
  SRCS Base64SWAR.cpp
  HEADERS Base64SWAR.h
  DEPS
    folly_detail_base64_detail_base64_constants
    folly_detail_base64_detail_base64_hidden_constants
    folly_detail_base64_detail_base64_scalar
    folly_portability
  EXPORTED_DEPS folly_detail_base64_detail_base64_common
)
