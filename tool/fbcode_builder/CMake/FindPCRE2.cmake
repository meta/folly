# Copyright (c) Facebook, Inc. and its affiliates.
include(FindPackageHandleStandardArgs)
find_path(PCRE2_INCLUDE_DIR NAMES pcre2.h)
find_library(PCRE2_LIBRARY NAMES pcre2-8)
find_package_handle_standard_args(
  PCRE2
  DEFAULT_MSG
  PCRE2_LIBRARY
  PCRE2_INCLUDE_DIR
)
set(PCRE2_DEFINES "PCRE2_CODE_UNIT_WIDTH=8")
mark_as_advanced(PCRE2_INCLUDE_DIR PCRE2_LIBRARY PCRE2_DEFINES)
