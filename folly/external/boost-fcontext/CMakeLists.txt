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

if (FOLLY_FIBERS_USE_BOOST_CONTEXT)
  folly_add_library(
    NAME fcontext
    HEADERS fcontext.h
    EXTERNAL_DEPS Boost::context
  )
  return ()
endif ()

# On Apple platforms the wrappers pick the variant for each architecture in
# CMAKE_OSX_ARCHITECTURES, which may differ from CMAKE_SYSTEM_PROCESSOR.
if (APPLE)
  set(abi macho_gas.S)
elseif (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
  set(abi x86_64_sysv_elf_gas.S)
else ()
  set(abi arm64_aapcs_elf_gas.S)
endif ()

folly_add_library(
  NAME fcontext
  SRCS make_${abi} jump_${abi}
  HEADERS fcontext.h
)
