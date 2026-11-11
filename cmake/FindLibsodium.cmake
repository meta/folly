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

find_path(LIBSODIUM_INCLUDE_DIR NAMES sodium.h)
mark_as_advanced(LIBSODIUM_INCLUDE_DIR)

find_library(LIBSODIUM_LIBRARY NAMES sodium)
mark_as_advanced(LIBSODIUM_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libsodium REQUIRED_VARS LIBSODIUM_LIBRARY
                                                          LIBSODIUM_INCLUDE_DIR)

if (LIBSODIUM_FOUND)
  set(LIBSODIUM_LIBRARIES ${LIBSODIUM_LIBRARY})
  set(LIBSODIUM_INCLUDE_DIRS ${LIBSODIUM_INCLUDE_DIR})
  message(STATUS "Found Libsodium: ${LIBSODIUM_LIBRARY}")
endif ()
