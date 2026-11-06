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

find_path(LIBIBERTY_INCLUDE_DIR NAMES libiberty.h PATH_SUFFIXES libiberty)
mark_as_advanced(LIBIBERTY_INCLUDE_DIR)

find_library(LIBIBERTY_LIBRARY NAMES iberty)
mark_as_advanced(LIBIBERTY_LIBRARY)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(
  Libiberty
  REQUIRED_VARS LIBIBERTY_LIBRARY LIBIBERTY_INCLUDE_DIR)

if(LIBIBERTY_FOUND)
  set(LIBIBERTY_LIBRARIES ${LIBIBERTY_LIBRARY})
  set(LIBIBERTY_INCLUDE_DIRS ${LIBIBERTY_INCLUDE_DIR})
endif()
