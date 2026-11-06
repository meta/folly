# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

find_path(CARES_INCLUDE_DIR NAMES ares.h)
find_library(CARES_LIBRARIES NAMES cares)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Cares DEFAULT_MSG CARES_LIBRARIES CARES_INCLUDE_DIR)

mark_as_advanced(
  CARES_LIBRARIES
  CARES_INCLUDE_DIR
)

if(NOT TARGET cares)
    if("${CARES_LIBRARIES}" MATCHES ".*.a$")
    add_library(cares STATIC IMPORTED)
    else()
    add_library(cares SHARED IMPORTED)
    endif()
    set_target_properties(
        cares
        PROPERTIES
            IMPORTED_LOCATION ${CARES_LIBRARIES}
            INTERFACE_INCLUDE_DIRECTORIES ${CARES_INCLUDE_DIR}
    )
endif()
