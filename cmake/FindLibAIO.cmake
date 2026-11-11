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

find_path(LIBAIO_INCLUDE_DIR NAMES libaio.h)
mark_as_advanced(LIBAIO_INCLUDE_DIR)

find_library(LIBAIO_LIBRARY NAMES aio)
mark_as_advanced(LIBAIO_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LibAIO REQUIRED_VARS LIBAIO_LIBRARY
                                                       LIBAIO_INCLUDE_DIR)

if (LIBAIO_FOUND)
  set(LIBAIO_LIBRARIES ${LIBAIO_LIBRARY})
  set(LIBAIO_INCLUDE_DIRS ${LIBAIO_INCLUDE_DIR})
endif ()
