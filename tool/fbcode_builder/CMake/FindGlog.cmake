# Copyright (c) Facebook, Inc. and its affiliates.
# - Try to find Glog
# Once done, this will define
#
# GLOG_FOUND - system has Glog
# GLOG_INCLUDE_DIRS - the Glog include directories
# GLOG_LIBRARIES - link these to use Glog

include(FindPackageHandleStandardArgs)
include(SelectLibraryConfigurations)

find_library(GLOG_LIBRARY_RELEASE glog
  PATHS ${GLOG_LIBRARYDIR})
find_library(GLOG_LIBRARY_DEBUG glogd
  PATHS ${GLOG_LIBRARYDIR})

find_path(GLOG_INCLUDE_DIR glog/logging.h
  PATHS ${GLOG_INCLUDEDIR})

select_library_configurations(GLOG)

find_package_handle_standard_args(Glog DEFAULT_MSG
  GLOG_LIBRARY
  GLOG_INCLUDE_DIR)

mark_as_advanced(
  GLOG_LIBRARY
  GLOG_INCLUDE_DIR)

set(GLOG_LIBRARIES ${GLOG_LIBRARY})
set(GLOG_INCLUDE_DIRS ${GLOG_INCLUDE_DIR})

if (GLOG_FOUND AND NOT TARGET glog::glog)
  add_library(glog::glog UNKNOWN IMPORTED)
  set_target_properties(glog::glog PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${GLOG_INCLUDE_DIRS}")
  set_target_properties(glog::glog PROPERTIES IMPORTED_LINK_INTERFACE_LANGUAGES "C" IMPORTED_LOCATION "${GLOG_LIBRARIES}")
  set_target_properties(glog::glog PROPERTIES
    INTERFACE_COMPILE_DEFINITIONS "GLOG_USE_GLOG_EXPORT")

  find_package(Gflags)
  if(GFLAGS_FOUND)
    message(STATUS "Found gflags as a dependency of glog::glog, include=${LIBGFLAGS_INCLUDE_DIR}, libs=${LIBGFLAGS_LIBRARY}")
    set_property(TARGET glog::glog APPEND PROPERTY IMPORTED_LINK_INTERFACE_LIBRARIES ${LIBGFLAGS_LIBRARY})
  endif()

  find_package(LibUnwind)
  if(LIBUNWIND_FOUND)
    message(STATUS "Found LibUnwind as a dependency of glog::glog, include=${LIBUNWIND_INCLUDE_DIR}, libs=${LIBUNWIND_LIBRARY}")
    set_property(TARGET glog::glog APPEND PROPERTY IMPORTED_LINK_INTERFACE_LIBRARIES ${LIBUNWIND_LIBRARY})
  endif()
endif()
