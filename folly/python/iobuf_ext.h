/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#if defined(__APPLE__)
#include <TargetConditionals.h> // @manual
#endif

#if defined(__APPLE__) && (TARGET_OS_SIMULATOR || TARGET_OS_IPHONE)
// Apple mobile OSes ship no Python interpreter.
#else
#include <Python.h>
#endif

#include <folly/Executor.h>
#include <folly/io/IOBuf.h>

namespace folly::python {

#if !defined(__APPLE__) || !(TARGET_OS_SIMULATOR || TARGET_OS_IPHONE)
// Returns a C++ IOBuf that shares ownership of the given Python memoryview
// object. The C++ IOBuf can then be exposed and used as a Python IOBuf object.
std::unique_ptr<folly::IOBuf> iobuf_from_memoryview(
    folly::Executor* executor, PyObject* py_object, void* buf, uint64_t length);

#endif // Apple mobile OSes ship no Python interpreter

// Returns a C++ IOBuf that OWNS a copy of the data from buf. Unlike
// iobuf_from_memoryview, this does not hold any reference to Python objects,
// making it safe for cases where the IOBuf may outlive the Python buffer.
std::unique_ptr<folly::IOBuf> iobuf_copy_from_buffer(
    const void* buf, std::size_t length);

std::unique_ptr<folly::IOBuf> create_iobuf(std::size_t capacity);

} // namespace folly::python
