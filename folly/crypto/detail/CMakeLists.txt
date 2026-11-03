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
  NAME lt_hash_internal
  HEADERS LtHashInternal.h
  EXPORTED_DEPS folly_range
)

folly_add_library(
  NAME math_operation_simple
  SRCS MathOperation_Simple.cpp
  DEPS
    folly_crypto_detail_lt_hash_internal
    folly_lang_bits
    folly_memory
)

# Apply SSE2/AVX2 compile flags on x86
if (IS_X86_64_ARCH)
  target_compile_options(
    folly_crypto_detail_math_operation_simple_obj
    PRIVATE
      -mno-avx
      -mno-avx2
      -mno-sse2
  )
endif ()

folly_add_library(
  NAME math_operation_sse2
  SRCS MathOperation_SSE2.cpp
  DEPS
    folly_crypto_detail_lt_hash_internal
    folly_ext_sodium
    folly_lang_bits
    folly_memory
)

if (IS_X86_64_ARCH)
  target_compile_options(
    folly_crypto_detail_math_operation_sse2_obj
    PRIVATE
      -mno-avx
      -mno-avx2
      -msse2
  )
endif ()

folly_add_library(
  NAME math_operation_avx2
  SRCS MathOperation_AVX2.cpp
  DEPS
    folly_crypto_detail_lt_hash_internal
    folly_ext_sodium
    folly_lang_bits
    folly_memory
)

if (IS_X86_64_ARCH)
  target_compile_options(
    folly_crypto_detail_math_operation_avx2_obj
    PRIVATE
      -mavx
      -mavx2
      -msse2
  )
endif ()

folly_add_library(
  NAME math_operation_sse2_disable
  EXCLUDE_FROM_MONOLITH
  SRCS MathOperation_SSE2.cpp
  DEPS
    folly_crypto_detail_lt_hash_internal
    folly_ext_sodium
    folly_lang_bits
    folly_memory
)

folly_add_library(
  NAME math_operation_avx2_disable
  EXCLUDE_FROM_MONOLITH
  SRCS MathOperation_AVX2.cpp
  DEPS
    folly_crypto_detail_lt_hash_internal
    folly_ext_sodium
    folly_lang_bits
    folly_memory
)
