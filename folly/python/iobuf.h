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

#include <folly/io/IOBuf.h>

namespace folly::python {

#if !defined(__APPLE__) || !(TARGET_OS_SIMULATOR || TARGET_OS_IPHONE)
/**
 * Returns a copy of the C++ IOBuf underlying a Python IOBuf object. In
 * practice, this makes it possible to pass Python IOBuf objects across the
 * Python/C++ boundary in pybind and other non-Cython extension code.
 */
folly::IOBuf iobuf_from_python_iobuf(PyObject* iobuf);
/**
 * Like the above, but more efficient when stack-allocated IOBuf not needed.
 * On python error, returns nullptr, so result must be checked.
 * This allows caller to control handling via C++ exception or python error.
 */
std::unique_ptr<folly::IOBuf> iobuf_ptr_from_python_iobuf(PyObject* iobuf);

/**
 * Constructs a python IOBuf object, callable from C python extension code.
 * Returns nullptr on python error; caller is responsible for error handling
 * and for ensuring no further calls to python C api if PyErr set.
 */
PyObject* make_python_iobuf(std::unique_ptr<folly::IOBuf> iobuf);

#endif // Apple mobile OSes ship no Python interpreter

inline bool check_iobuf_equal(const folly::IOBuf* a, const folly::IOBuf* b) {
  return folly::IOBufEqualTo{}(a, b);
}

inline bool check_iobuf_less(const folly::IOBuf* a, const folly::IOBuf* b) {
  return folly::IOBufLess{}(a, b);
}

} // namespace folly::python
