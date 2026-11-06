# Copyright (c) Facebook, Inc. and its affiliates.
# Find libgmock
#
#  LIBGMOCK_DEFINES     - List of defines when using libgmock.
#  LIBGMOCK_INCLUDE_DIR - where to find gmock/gmock.h, etc.
#  LIBGMOCK_LIBRARIES   - List of libraries when using libgmock.
#  LIBGMOCK_FOUND       - True if libgmock found.

IF (LIBGMOCK_INCLUDE_DIR)
  # Already in cache, be silent
  SET(LIBGMOCK_FIND_QUIETLY TRUE)
ENDIF ()

find_package(GTest CONFIG QUIET)
if (TARGET GTest::gmock)
  get_target_property(LIBGMOCK_DEFINES GTest::gtest INTERFACE_COMPILE_DEFINITIONS)
  if (NOT ${LIBGMOCK_DEFINES})
    # Explicitly set to empty string if not found to avoid it being
    # set to NOTFOUND and breaking compilation
    set(LIBGMOCK_DEFINES "")
  endif()
  get_target_property(LIBGMOCK_INCLUDE_DIR GTest::gtest INTERFACE_INCLUDE_DIRECTORIES)
  set(LIBGMOCK_LIBRARIES GTest::gmock_main GTest::gmock GTest::gtest)
  set(LIBGMOCK_FOUND ON)
  message(STATUS "Found gmock via config, defines=${LIBGMOCK_DEFINES}, include=${LIBGMOCK_INCLUDE_DIR}, libs=${LIBGMOCK_LIBRARIES}")
else()

  FIND_PATH(LIBGMOCK_INCLUDE_DIR gmock/gmock.h)

  FIND_LIBRARY(LIBGMOCK_MAIN_LIBRARY_DEBUG NAMES gmock_maind)
  FIND_LIBRARY(LIBGMOCK_MAIN_LIBRARY_RELEASE NAMES gmock_main)
  FIND_LIBRARY(LIBGMOCK_LIBRARY_DEBUG NAMES gmockd)
  FIND_LIBRARY(LIBGMOCK_LIBRARY_RELEASE NAMES gmock)
  FIND_LIBRARY(LIBGTEST_LIBRARY_DEBUG NAMES gtestd)
  FIND_LIBRARY(LIBGTEST_LIBRARY_RELEASE NAMES gtest)

  find_package(Threads REQUIRED)
  INCLUDE(SelectLibraryConfigurations)
  SELECT_LIBRARY_CONFIGURATIONS(LIBGMOCK_MAIN)
  SELECT_LIBRARY_CONFIGURATIONS(LIBGMOCK)
  SELECT_LIBRARY_CONFIGURATIONS(LIBGTEST)

  set(LIBGMOCK_LIBRARIES
    ${LIBGMOCK_MAIN_LIBRARY}
    ${LIBGMOCK_LIBRARY}
    ${LIBGTEST_LIBRARY}
    Threads::Threads
  )

  if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # The GTEST_LINKED_AS_SHARED_LIBRARY macro must be set properly on Windows.
    #
    # There isn't currently an easy way to determine if a library was compiled as
    # a shared library on Windows, so just assume we've been built against a
    # shared build of gmock for now.
    SET(LIBGMOCK_DEFINES "GTEST_LINKED_AS_SHARED_LIBRARY=1" CACHE STRING "")
  endif()

  # handle the QUIETLY and REQUIRED arguments and set LIBGMOCK_FOUND to TRUE if
  # all listed variables are TRUE
  INCLUDE(FindPackageHandleStandardArgs)
  FIND_PACKAGE_HANDLE_STANDARD_ARGS(
    GMock
    DEFAULT_MSG
    LIBGMOCK_MAIN_LIBRARY
    LIBGMOCK_LIBRARY
    LIBGTEST_LIBRARY
    LIBGMOCK_LIBRARIES
    LIBGMOCK_INCLUDE_DIR
  )

  MARK_AS_ADVANCED(
    LIBGMOCK_DEFINES
    LIBGMOCK_MAIN_LIBRARY
    LIBGMOCK_LIBRARY
    LIBGTEST_LIBRARY
    LIBGMOCK_LIBRARIES
    LIBGMOCK_INCLUDE_DIR
  )
endif()
