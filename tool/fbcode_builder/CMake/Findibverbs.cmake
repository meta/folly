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

# Find the ibverbs libraries
#
# The following variables are optionally searched for defaults
#  IBVERBS_ROOT_DIR: Base directory where all ibverbs components are found
#  IBVERBS_INCLUDE_DIR: Directory where ibverbs headers are found
#  IBVERBS_LIB_DIR: Directory where ibverbs libraries are found

# The following are set after configuration is done:
#  IBVERBS_FOUND
#  IBVERBS_INCLUDE_DIRS
#  IBVERBS_LIBRARIES
#  IBVERBS_VERSION

find_path(IBVERBS_INCLUDE_DIRS
  NAMES infiniband/verbs.h
  HINTS
  ${IBVERBS_INCLUDE_DIR}
  ${IBVERBS_ROOT_DIR}
  ${IBVERBS_ROOT_DIR}/include)

find_library(IBVERBS_LIBRARIES
  NAMES ibverbs
  HINTS
  ${IBVERBS_LIB_DIR}
  ${IBVERBS_ROOT_DIR}
  ${IBVERBS_ROOT_DIR}/lib)

# Try to determine the rdma-core version
if(IBVERBS_INCLUDE_DIRS AND IBVERBS_LIBRARIES)
  # First try using pkg-config if available
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_RDMA_CORE QUIET rdma-core)
    if(PC_RDMA_CORE_VERSION)
      set(IBVERBS_VERSION ${PC_RDMA_CORE_VERSION})
    endif()
  endif()

  # If pkg-config didn't work, try to extract version from library filename
  # According to rdma-core Documentation/versioning.md:
  # Library filename format:
  #   libibverbs.so.SONAME.ABI.PACKAGE_VERSION_MAIN[.PACKAGE_VERSION_BRANCH]
  # Where:
  #   - SONAME: Major version (1st field)
  #   - ABI: ABI version number (2nd field)
  #   - PACKAGE_VERSION_MAIN: Main package version (3rd field)
  #   - PACKAGE_VERSION_BRANCH: Optional counter for branched stable
  #     releases (4th field, part of PACKAGE_VERSION)
  # Example: libibverbs.so.1.14.57.0 → SONAME=1, ABI=14,
  #   PACKAGE_VERSION=57.0
  if(NOT IBVERBS_VERSION)
    # Get the real path of the library (follows symlinks)
    get_filename_component(IBVERBS_REAL_PATH "${IBVERBS_LIBRARIES}" REALPATH)
    get_filename_component(IBVERBS_LIB_NAME "${IBVERBS_REAL_PATH}" NAME)

    # Extract version from filename
    if(IBVERBS_LIB_NAME MATCHES
       "libibverbs\\.so\\.([0-9]+)\\.([0-9]+)\\.([0-9]+)\\.([0-9]+)")
      # Four-component version: PACKAGE_VERSION_MAIN.PACKAGE_VERSION_BRANCH
      set(IBVERBS_VERSION_MAJOR ${CMAKE_MATCH_3})
      set(IBVERBS_VERSION_MINOR ${CMAKE_MATCH_4})
      set(IBVERBS_VERSION "${IBVERBS_VERSION_MAJOR}.${IBVERBS_VERSION_MINOR}")
    elseif(IBVERBS_LIB_NAME MATCHES
           "libibverbs\\.so\\.([0-9]+)\\.([0-9]+)\\.([0-9]+)")
      # Three-component version: PACKAGE_VERSION_MAIN only
      set(IBVERBS_VERSION_MAJOR ${CMAKE_MATCH_3})
      set(IBVERBS_VERSION "${IBVERBS_VERSION_MAJOR}.0")
    else()
      # If we can't parse the filename, set to empty string
      # Feature detection will be done in CMakeLists.txt
      set(IBVERBS_VERSION "")
    endif()
  endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ibverbs
  REQUIRED_VARS IBVERBS_INCLUDE_DIRS IBVERBS_LIBRARIES
  VERSION_VAR IBVERBS_VERSION)
mark_as_advanced(IBVERBS_INCLUDE_DIRS IBVERBS_LIBRARIES IBVERBS_VERSION)
