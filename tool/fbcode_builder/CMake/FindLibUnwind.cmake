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

include(FindPackageHandleStandardArgs)

# Prefer pkg-config: picks up transitive deps (e.g. lzma, zlib) that
# static libunwind needs but a bare find_library would miss.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_LIBUNWIND QUIET libunwind)
endif()

if(PC_LIBUNWIND_FOUND)
  find_path(LIBUNWIND_INCLUDE_DIR NAMES libunwind.h
    HINTS ${PC_LIBUNWIND_INCLUDE_DIRS}
    PATH_SUFFIXES libunwind)
  mark_as_advanced(LIBUNWIND_INCLUDE_DIR)

  # Resolve each library from the static set (Libs + Libs.private) to a
  # full path.  This gives the linker everything it needs for a fully-static
  # link without leaking imported targets through cmake exports.
  set(LIBUNWIND_LIBRARIES "")
  foreach(_lib IN LISTS PC_LIBUNWIND_STATIC_LIBRARIES)
    find_library(_libunwind_dep_${_lib} NAMES ${_lib}
      HINTS ${PC_LIBUNWIND_STATIC_LIBRARY_DIRS})
    if(_libunwind_dep_${_lib})
      list(APPEND LIBUNWIND_LIBRARIES ${_libunwind_dep_${_lib}})
    else()
      # Fall back to bare name; the linker will resolve -l<name>.
      list(APPEND LIBUNWIND_LIBRARIES ${_lib})
    endif()
    unset(_libunwind_dep_${_lib} CACHE)
  endforeach()
  set(LIBUNWIND_LIBRARY "${LIBUNWIND_LIBRARIES}")

  FIND_PACKAGE_HANDLE_STANDARD_ARGS(LibUnwind
    REQUIRED_VARS LIBUNWIND_LIBRARIES LIBUNWIND_INCLUDE_DIR)
else()
  # Fallback for systems without pkg-config.
  # When using prepackaged LLVM libunwind on Ubuntu, its includes are
  # installed in a subdirectory.
  find_path(LIBUNWIND_INCLUDE_DIR NAMES libunwind.h PATH_SUFFIXES libunwind)
  mark_as_advanced(LIBUNWIND_INCLUDE_DIR)

  find_library(LIBUNWIND_LIBRARY NAMES unwind)
  mark_as_advanced(LIBUNWIND_LIBRARY)

  FIND_PACKAGE_HANDLE_STANDARD_ARGS(LibUnwind
    REQUIRED_VARS LIBUNWIND_LIBRARY LIBUNWIND_INCLUDE_DIR)
endif()

if(LibUnwind_FOUND)
  if(NOT LIBUNWIND_LIBRARIES)
    set(LIBUNWIND_LIBRARIES ${LIBUNWIND_LIBRARY})
  endif()
  set(LIBUNWIND_INCLUDE_DIRS ${LIBUNWIND_INCLUDE_DIR})
endif()
