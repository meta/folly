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

# distutils: language=c++

from folly.iobuf cimport from_unique_ptr, createChain, IOBuf, WritableIOBuf, writable_from_unique_ptr
from cython.view cimport memoryview
from builtins import memoryview as py_memoryview

def get_empty_chain():
    return from_unique_ptr(createChain(1024, 128))

def get_empty_writable_chain():
    return writable_from_unique_ptr(createChain(1024, 128))

def make_chain(data):
    cdef IOBuf head = data.pop(0)
    # Make a new chain
    head = from_unique_ptr(head.c_clone())
    cdef IOBuf tbuf
    cdef IOBuf last = head
    for tbuf in data:
        last._this.insertAfterThisOne(tbuf.c_clone())
        last = last.next
    return head

def make_writable_chain(data):
    cdef WritableIOBuf head = data.pop(0)
    # Make a new chain
    head = writable_from_unique_ptr(head.c_clone())
    cdef WritableIOBuf tbuf
    cdef WritableIOBuf last = head
    for tbuf in data:
        last._this.insertAfterThisOne(tbuf.c_clone())
        last = last.next
    return head

def to_uppercase_string(iobuf: object) -> str:
    return to_uppercase_string_cpp(iobuf).decode("utf-8")

def to_uppercase_string_heap(iobuf: object) -> str:
    return to_uppercase_string_cpp_heap(iobuf).decode("utf-8")

def wrap_and_delayed_free(mv: py_memoryview, free_delay_ms: int):
    cdef memoryview view = memoryview(mv, PyBUF_C_CONTIGUOUS)
    wrap_and_delayed_free_cpp(
        <PyObject*>mv,
        view.view.buf,
        view.view.len,
        free_delay_ms)
