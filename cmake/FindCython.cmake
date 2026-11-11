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

# Find Cython
#
# This module sets the following variables:
# - Cython_FOUND
# - CYTHON_EXE
# - CYTHON_VERSION_STRING
#
find_program(CYTHON_EXE NAMES cython cython3)
if (CYTHON_EXE)
  execute_process(
    COMMAND ${CYTHON_EXE} --version
    RESULT_VARIABLE _cython_retcode
    OUTPUT_VARIABLE _cython_output
    ERROR_VARIABLE _cython_output
    OUTPUT_STRIP_TRAILING_WHITESPACE)

  if (${_cython_retcode} EQUAL 0)
    separate_arguments(_cython_output)
    list(GET _cython_output -1 CYTHON_VERSION_STRING)
    message(STATUS "Found Cython Version ${CYTHON_VERSION_STRING}")
  else ()
    message(STATUS "Failed to get Cython version")
  endif ()
else ()
  message(STATUS "Cython not found")
endif ()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  Cython
  REQUIRED_VARS CYTHON_EXE CYTHON_VERSION_STRING
  VERSION_VAR CYTHON_VERSION_STRING)
