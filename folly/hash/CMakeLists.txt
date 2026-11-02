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
  NAME checksum
  SRCS Checksum.cpp
  HEADERS Checksum.h
  DEPS
    folly_cpu_id
    folly_detail_traponavx512
    folly_external_fast-crc32_avx512_crc32c_v8s3x4
    folly_external_fast-crc32_neon_crc32c_v3s4x2e_v2
    folly_external_fast-crc32_neon_eor3_crc32_v8s2x4e_s1x2
    folly_external_fast-crc32_neon_eor3_crc32c_v8s2x4e_s2x1
    folly_external_fast-crc32_sse_crc32c_v8s3x3
    folly_hash_detail_checksum_detail
)

# Add fast-crc32 dependencies - all built on all platforms (source has stubs)
target_link_libraries(
  folly_hash_checksum_obj
  PRIVATE
    folly_external_fast-crc32_avx512_crc32c_v8s3x4_obj
    folly_external_fast-crc32_sse_crc32c_v8s3x3_obj
    folly_external_fast-crc32_neon_crc32c_v3s4x2e_v2_obj
    folly_external_fast-crc32_neon_eor3_crc32_v8s2x4e_s1x2_obj
    folly_external_fast-crc32_neon_eor3_crc32c_v8s2x4e_s2x1_obj
)

folly_add_library(
  NAME farm_hash
  HEADERS FarmHash.h
  EXPORTED_DEPS folly_external_farmhash_farmhash
)

folly_add_library(
  NAME hash
  HEADERS Hash.h
  EXPORTED_DEPS
    folly_c_portability
    folly_hash_fnv_hash
    folly_hash_hsieh_hash
    folly_hash_murmur_hash
    folly_hash_spooky_hash_v1
    folly_hash_spooky_hash_v2
    folly_lang_bits
    folly_lang_cstring_view
    folly_portability
    folly_traits
    folly_utility
)

folly_add_library(
  NAME rapidhash
  HEADERS rapidhash.h
  EXPORTED_DEPS folly_external_rapidhash_rapidhash
)

folly_add_library(
  NAME spooky_hash_v1
  SRCS SpookyHashV1.cpp
  HEADERS SpookyHashV1.h
)

folly_add_library(
  NAME spooky_hash_v2
  SRCS SpookyHashV2.cpp
  HEADERS SpookyHashV2.h
  EXPORTED_DEPS
    folly_c_portability
    folly_lang_c_string
    folly_portability
)

folly_add_library(
  NAME traits
  HEADERS traits.h
  EXPORTED_DEPS folly_traits
)

folly_add_library(
  NAME murmur_hash
  HEADERS MurmurHash.h
  EXPORTED_DEPS
    folly_c_portability
    folly_lang_bits
    folly_portability_constexpr
)

folly_add_library(
  NAME fnv_hash
  HEADERS FnvHash.h
)

folly_add_library(
  NAME hsieh_hash
  HEADERS HsiehHash.h
  EXPORTED_DEPS folly_lang_bits
)

if (FOLLY_HAVE_OPENSSL)
  folly_add_library(
    NAME unique_hash_key
    SRCS UniqueHashKey.cpp
    HEADERS UniqueHashKey.h
    DEPS
      folly_lang_cast
      folly_portability_openssl
      folly_portability_unistd
    EXPORTED_DEPS
      folly_portability_config
      folly_traits
  )
else ()
  folly_exclude_headers(UniqueHashKey.h)
endif ()

add_subdirectory(detail)
