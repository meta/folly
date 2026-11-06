# Copyright (c) Facebook, Inc. and its affiliates.
# Find libgflags.
# There's a lot of compatibility cruft going on in here, both
# to deal with changes across the FB consumers of this and also
# to deal with variances in behavior of cmake itself.
#
# Since this file is named FindGflags.cmake the cmake convention
# is for the module to export both GFLAGS_FOUND and Gflags_FOUND.
# The convention expected by consumers is that we export the
# following variables, even though these do not match the cmake
# conventions:
#
#  LIBGFLAGS_INCLUDE_DIR - where to find gflags/gflags.h, etc.
#  LIBGFLAGS_LIBRARY     - List of libraries when using libgflags.
#  LIBGFLAGS_FOUND       - True if libgflags found.
#
# We need to be able to locate gflags both from an installed
# cmake config file and just from the raw headers and libs, so
# test for the former and then the latter, and then stick
# the results together and export them into the variables
# listed above.
#
# For forwards compatibility, we export the following variables:
#
#  gflags_INCLUDE_DIR - where to find gflags/gflags.h, etc.
#  gflags_TARGET / GFLAGS_TARGET / gflags_LIBRARIES
#                     - List of libraries when using libgflags.
#  gflags_FOUND       - True if libgflags found.
#

IF (LIBGFLAGS_INCLUDE_DIR)
  # Already in cache, be silent
  SET(Gflags_FIND_QUIETLY TRUE)
ENDIF ()

find_package(gflags CONFIG QUIET)
if (gflags_FOUND)
  if (NOT Gflags_FIND_QUIETLY)
    message(STATUS "Found gflags from package config ${gflags_CONFIG}")
  endif()
  # Re-export the config-specified libs with our local names
  set(LIBGFLAGS_LIBRARY ${gflags_LIBRARIES})
  set(LIBGFLAGS_INCLUDE_DIR ${gflags_INCLUDE_DIR})
  if(NOT EXISTS "${gflags_INCLUDE_DIR}")
    # The gflags-devel RPM on recent RedHat-based systems is somewhat broken.
    # RedHat symlinks /lib64 to /usr/lib64, and this breaks some of the
    # relative path computation performed in gflags-config.cmake.  The package
    # config file ends up being found via /lib64, but the relative path
    # computation it does only works if it was found in /usr/lib64.
    # If gflags_INCLUDE_DIR does not actually exist, simply default it to
    # /usr/include on these systems.
    set(LIBGFLAGS_INCLUDE_DIR "/usr/include")
    set(GFLAGS_INCLUDE_DIR "/usr/include")
  endif()
  set(LIBGFLAGS_FOUND ${gflags_FOUND})
  # cmake module compat
  set(GFLAGS_FOUND ${gflags_FOUND})
  set(Gflags_FOUND ${gflags_FOUND})
else()
  FIND_PATH(LIBGFLAGS_INCLUDE_DIR gflags/gflags.h)

  FIND_LIBRARY(LIBGFLAGS_LIBRARY_DEBUG NAMES gflagsd gflags_staticd)
  FIND_LIBRARY(LIBGFLAGS_LIBRARY_RELEASE NAMES gflags gflags_static)

  INCLUDE(SelectLibraryConfigurations)
  SELECT_LIBRARY_CONFIGURATIONS(LIBGFLAGS)

  # handle the QUIETLY and REQUIRED arguments and set LIBGFLAGS_FOUND to TRUE if
  # all listed variables are TRUE
  INCLUDE(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(Gflags DEFAULT_MSG LIBGFLAGS_LIBRARY LIBGFLAGS_INCLUDE_DIR)
  # cmake module compat
  set(Gflags_FOUND ${GFLAGS_FOUND})
  # compat with some existing FindGflags consumers
  set(LIBGFLAGS_FOUND ${GFLAGS_FOUND})

  # Compat with the gflags CONFIG based detection
  set(gflags_FOUND ${GFLAGS_FOUND})
  set(gflags_INCLUDE_DIR ${LIBGFLAGS_INCLUDE_DIR})
  set(gflags_LIBRARIES ${LIBGFLAGS_LIBRARY})
  set(GFLAGS_TARGET ${LIBGFLAGS_LIBRARY})
  set(gflags_TARGET ${LIBGFLAGS_LIBRARY})

  MARK_AS_ADVANCED(LIBGFLAGS_LIBRARY LIBGFLAGS_INCLUDE_DIR)
endif()

# Compat with the gflags CONFIG based detection
if (LIBGFLAGS_FOUND AND NOT TARGET gflags)
  add_library(gflags UNKNOWN IMPORTED)
  if(TARGET gflags-shared)
    # If the installed gflags CMake package config defines a gflags-shared
    # target but not gflags, just make the gflags target that we define
    # depend on the gflags-shared target.
    target_link_libraries(gflags INTERFACE gflags-shared)
    # Export LIBGFLAGS_LIBRARY as the gflags-shared target in this case.
    set(LIBGFLAGS_LIBRARY gflags-shared)
  else()
    set_target_properties(
      gflags
      PROPERTIES
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION "${LIBGFLAGS_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBGFLAGS_INCLUDE_DIR}"
    )
  endif()
endif()
