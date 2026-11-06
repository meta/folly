# Copyright (c) Facebook, Inc. and its affiliates.
include(FindPackageHandleStandardArgs)
find_path(PCRE_INCLUDE_DIR NAMES pcre.h)
find_library(PCRE_LIBRARY NAMES pcre)
find_package_handle_standard_args(
  PCRE
  DEFAULT_MSG
  PCRE_LIBRARY
  PCRE_INCLUDE_DIR
)
mark_as_advanced(PCRE_INCLUDE_DIR PCRE_LIBRARY)
