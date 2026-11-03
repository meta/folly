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

#include <folly/python/iobuf.h>

#include <folly/python/import.h>

#if defined(__APPLE__)
#include <TargetConditionals.h> // @manual
#endif

#if defined(__APPLE__) && (TARGET_OS_SIMULATOR || TARGET_OS_IPHONE)
// Apple mobile OSes ship no Python interpreter. The generated API header and
// the Python-bound definitions below require it, so they are omitted here.
#else
#include <folly/python/iobuf_api.h> // @manual
#endif

namespace folly::python {

#if !defined(__APPLE__) || !(TARGET_OS_SIMULATOR || TARGET_OS_IPHONE)
constinit static python::import_cache import_folly__iobuf_{
    import_folly__iobuf, "import_folly__iobuf"};

folly::IOBuf iobuf_from_python_iobuf(PyObject* iobuf) {
  import_folly__iobuf_();
  return from_python_iobuf(iobuf);
}

std::unique_ptr<folly::IOBuf> iobuf_ptr_from_python_iobuf(PyObject* iobuf) {
  import_folly__iobuf_();
  return std::unique_ptr<folly::IOBuf>(ptr_from_python_iobuf(iobuf));
}

PyObject* make_python_iobuf(std::unique_ptr<folly::IOBuf> iobuf) {
  import_folly__iobuf_();
  return python_iobuf_from_ptr(std::move(iobuf));
}
#endif

} // namespace folly::python
