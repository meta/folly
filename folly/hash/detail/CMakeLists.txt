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
  NAME checksum_detail
  SRCS
    ChecksumDetail.cpp
    Crc32CombineDetail.cpp
    Crc32cDetail.cpp
  HEADERS ChecksumDetail.h
  DEPS
    folly_bits
    folly_cpp_attributes
    folly_cpu_id
    folly_detail_traponavx512
    folly_external_nvidia_hash_detail_crc32c_detail
  EXPORTED_DEPS
    folly_external_nvidia_hash_checksum
    folly_external_nvidia_hash_detail_crc32c_detail
    folly_portability
)

# Apply -mpclmul flag on x86 if supported (required for CRC32 intrinsics)
if (COMPILER_HAS_M_PCLMUL)
  target_compile_options(folly_hash_detail_checksum_detail_obj PRIVATE -mpclmul)
endif ()

folly_add_library(
  NAME random_seed
  HEADERS RandomSeed.h
  EXPORTED_DEPS folly_portability
)
