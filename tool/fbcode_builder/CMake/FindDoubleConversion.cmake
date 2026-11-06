# Copyright (c) Meta Platforms, Inc. and affiliates.

# Finds libdouble-conversion.
#
# This module defines:
# DOUBLE_CONVERSION_INCLUDE_DIR
# DOUBLE_CONVERSION_LIBRARY
#

find_path(DOUBLE_CONVERSION_INCLUDE_DIR double-conversion/double-conversion.h)
find_library(DOUBLE_CONVERSION_LIBRARY NAMES double-conversion)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  DoubleConversion
  DEFAULT_MSG
  DOUBLE_CONVERSION_LIBRARY DOUBLE_CONVERSION_INCLUDE_DIR)

mark_as_advanced(DOUBLE_CONVERSION_INCLUDE_DIR DOUBLE_CONVERSION_LIBRARY)
