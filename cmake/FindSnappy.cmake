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

# Find the Snappy libraries
#
# This module defines:
# SNAPPY_FOUND
# SNAPPY_INCLUDE_DIR
# SNAPPY_LIBRARY

find_path(SNAPPY_INCLUDE_DIR NAMES snappy.h)

find_library(SNAPPY_LIBRARY_DEBUG NAMES snappyd)
find_library(SNAPPY_LIBRARY_RELEASE NAMES snappy)

include(SelectLibraryConfigurations)
select_library_configurations(SNAPPY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Snappy DEFAULT_MSG SNAPPY_LIBRARY
                                  SNAPPY_INCLUDE_DIR)

mark_as_advanced(SNAPPY_INCLUDE_DIR SNAPPY_LIBRARY)
