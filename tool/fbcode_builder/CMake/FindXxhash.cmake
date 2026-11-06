# Copyright (c) Facebook, Inc. and its affiliates.
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

#
# - Try to find Facebook xxhash library
# This will define
# Xxhash_FOUND
# Xxhash_INCLUDE_DIR
# Xxhash_LIBRARY
# Xxhash::xxhash (imported target)
#

find_path(Xxhash_INCLUDE_DIR NAMES xxhash.h)

find_library(Xxhash_LIBRARY_RELEASE NAMES xxhash)

include(SelectLibraryConfigurations)
SELECT_LIBRARY_CONFIGURATIONS(Xxhash)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(
    Xxhash DEFAULT_MSG
    Xxhash_LIBRARY Xxhash_INCLUDE_DIR
)

if (Xxhash_FOUND)
  message(STATUS "Found xxhash: ${Xxhash_LIBRARY}")
endif()

if (Xxhash_FOUND AND NOT TARGET Xxhash::xxhash)
  add_library(Xxhash::xxhash UNKNOWN IMPORTED)
  set_target_properties(
    Xxhash::xxhash
    PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES "C"
               IMPORTED_LOCATION "${Xxhash_LIBRARY}"
               INTERFACE_INCLUDE_DIRECTORIES "${Xxhash_INCLUDE_DIR}")
endif()

mark_as_advanced(Xxhash_INCLUDE_DIR Xxhash_LIBRARY)
