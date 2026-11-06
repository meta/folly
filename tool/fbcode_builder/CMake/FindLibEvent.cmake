# Copyright (c) Facebook, Inc. and its affiliates.
# - Find LibEvent (a cross event library)
# This module defines
# LIBEVENT_INCLUDE_DIR, where to find LibEvent headers
# LIBEVENT_LIB, LibEvent libraries
# LibEvent_FOUND, If false, do not try to use libevent

set(LibEvent_EXTRA_PREFIXES /usr/local /opt/local "$ENV{HOME}")
foreach(prefix ${LibEvent_EXTRA_PREFIXES})
  list(APPEND LibEvent_INCLUDE_PATHS "${prefix}/include")
  list(APPEND LibEvent_LIB_PATHS "${prefix}/lib")
endforeach()

find_package(Libevent CONFIG QUIET)
if (TARGET event)
  # Re-export the config under our own names

  # Somewhat gross, but some vcpkg installed libevents have a relative
  # `include` path exported into LIBEVENT_INCLUDE_DIRS, which triggers
  # a cmake error because it resolves to the `include` dir within the
  # folly repo, which is not something cmake allows to be in the
  # INTERFACE_INCLUDE_DIRECTORIES.  Thankfully on such a system the
  # actual include directory is already part of the global include
  # directories, so we can just skip it.
  if (NOT "${LIBEVENT_INCLUDE_DIRS}" STREQUAL "include")
    set(LIBEVENT_INCLUDE_DIR ${LIBEVENT_INCLUDE_DIRS})
  else()
    set(LIBEVENT_INCLUDE_DIR)
  endif()

  # Unfortunately, with a bare target name `event`, downstream consumers
  # of the package that depends on `Libevent` located via CONFIG end
  # up exporting just a bare `event` in their libraries.  This is problematic
  # because this in interpreted as just `-levent` with no library path.
  # When libevent is not installed in the default installation prefix
  # this results in linker errors.
  # To resolve this, we ask cmake to lookup the full path to the library
  # and use that instead.
  cmake_policy(PUSH)
  if(POLICY CMP0026)
    # Allow reading the LOCATION property
    cmake_policy(SET CMP0026 OLD)
  endif()
  get_target_property(LIBEVENT_LIB event LOCATION)
  cmake_policy(POP)

  set(LibEvent_FOUND ${Libevent_FOUND})
  if (NOT LibEvent_FIND_QUIETLY)
    message(STATUS "Found libevent from package config include=${LIBEVENT_INCLUDE_DIRS} lib=${LIBEVENT_LIB}")
  endif()
else()
  find_path(LIBEVENT_INCLUDE_DIR event.h PATHS ${LibEvent_INCLUDE_PATHS})
  find_library(LIBEVENT_LIB NAMES event PATHS ${LibEvent_LIB_PATHS})

  if (LIBEVENT_LIB AND LIBEVENT_INCLUDE_DIR)
    set(LibEvent_FOUND TRUE)
    set(LIBEVENT_LIB ${LIBEVENT_LIB})
  else ()
    set(LibEvent_FOUND FALSE)
  endif ()

  if (LibEvent_FOUND)
    if (NOT LibEvent_FIND_QUIETLY)
      message(STATUS "Found libevent: ${LIBEVENT_LIB}")
    endif ()
  else ()
    if (LibEvent_FIND_REQUIRED)
      message(FATAL_ERROR "Could NOT find libevent.")
    endif ()
    message(STATUS "libevent NOT found.")
  endif ()

  mark_as_advanced(
    LIBEVENT_LIB
    LIBEVENT_INCLUDE_DIR
  )
endif()
