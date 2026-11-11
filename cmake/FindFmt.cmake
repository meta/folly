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

find_path(LIBFMT_INCLUDE_DIR fmt/core.h)
mark_as_advanced(LIBFMT_INCLUDE_DIR)

find_library(LIBFMT_LIBRARY NAMES fmt fmtd)
mark_as_advanced(LIBFMT_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Fmt DEFAULT_MSG LIBFMT_LIBRARY
                                  LIBFMT_INCLUDE_DIR)

if (FMT_FOUND)
  set(LIBFMT_LIBRARIES ${LIBFMT_LIBRARY})
  set(LIBFMT_INCLUDE_DIRS ${LIBFMT_INCLUDE_DIR})
  message(STATUS "Found {fmt}: ${LIBFMT_LIBRARY}")
  add_library(fmt::fmt UNKNOWN IMPORTED)
  set_target_properties(fmt::fmt PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                                            "${LIBFMT_INCLUDE_DIR}")
  set_target_properties(
    fmt::fmt PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES "C"
                        IMPORTED_LOCATION "${LIBFMT_LIBRARY}")
endif ()
