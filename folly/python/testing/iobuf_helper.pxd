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

from libcpp.memory cimport unique_ptr
from folly.iobuf cimport cIOBuf
from libcpp.string cimport string
from cpython.ref cimport PyObject
from libc.stdint cimport uint64_t, int32_t

cdef extern from "folly/python/test/IOBufTestUtils.h" namespace "folly::python":
    cdef string to_uppercase_string_cpp(object iobuf) except *
    cdef string to_uppercase_string_cpp_heap(object iobuf) except *
    void wrap_and_delayed_free_cpp(PyObject*, void*, uint64_t, int32_t) except *


cdef extern from "Python.h":
    cdef int PyBUF_C_CONTIGUOUS
