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

set(CMAKE_CXX_FLAGS_COMMON "-g -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${CMAKE_CXX_FLAGS_COMMON}")
set(CMAKE_CXX_FLAGS_RELEASE
    "${CMAKE_CXX_FLAGS_RELEASE} ${CMAKE_CXX_FLAGS_COMMON} -O3")

list(APPEND CMAKE_REQUIRED_DEFINITIONS "-D_GNU_SOURCE")
function (apply_folly_compile_options_to_target THETARGET)
  target_compile_definitions(${THETARGET} PRIVATE _REENTRANT _GNU_SOURCE)
  target_compile_options(
    ${THETARGET}
    PRIVATE -g
            -finput-charset=UTF-8
            -fsigned-char
            -Wall
            -Wno-deprecated
            -Wno-deprecated-declarations
            -Wno-sign-compare
            -Wno-unused
            -Wuninitialized
            -Wunused-label
            -Wunused-result
            ${FOLLY_CXX_FLAGS})
endfunction ()
