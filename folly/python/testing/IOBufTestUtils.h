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

#include <Python.h>

#include <memory>
#include <string>

#include <folly/io/IOBuf.h>

namespace folly::python {

std::string to_uppercase_string_cpp(PyObject* iobuf);
std::string to_uppercase_string_cpp_heap(PyObject* o_iobuf);

void wrap_and_delayed_free_cpp(
    PyObject* memoryview, void* buf, uint64_t length, int32_t delayMs);

} // namespace folly::python
