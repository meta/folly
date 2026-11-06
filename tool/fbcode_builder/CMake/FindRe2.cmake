# Copyright (c) Facebook, Inc. and its affiliates.
#
# This software may be used and distributed according to the terms of the
# GNU General Public License version 2.

# re2's package config, where re2 has one, defines re2::re2 with the abseil
# libraries that a static re2 needs and a bare find_library() leaves out.
find_package(re2 CONFIG QUIET)
if(TARGET re2::re2)
  set(RE2_LIBRARY re2::re2)
else()
  find_library(RE2_LIBRARY re2)
endif()
mark_as_advanced(RE2_LIBRARY)

find_path(RE2_INCLUDE_DIR NAMES re2/re2.h)
mark_as_advanced(RE2_INCLUDE_DIR)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(
     Re2
     REQUIRED_VARS RE2_LIBRARY RE2_INCLUDE_DIR)

if(RE2_FOUND)
  set(RE2_LIBRARY ${RE2_LIBRARY})
  set(RE2_INCLUDE_DIR, ${RE2_INCLUDE_DIR})
endif()
