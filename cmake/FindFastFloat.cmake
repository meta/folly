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

#
# - Try to find fast_float library
# This will define
# FASTFLOAT_FOUND
# FASTFLOAT_INCLUDE_DIR
#

find_path(FASTFLOAT_INCLUDE_DIR NAMES fast_float/fast_float.h)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FastFloat DEFAULT_MSG FASTFLOAT_INCLUDE_DIR)

mark_as_advanced(FASTFLOAT_INCLUDE_DIR)
