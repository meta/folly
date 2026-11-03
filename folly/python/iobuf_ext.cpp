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

#include <cstring>

#include <folly/python/Weak.h>
#include <folly/python/iobuf_ext.h>

#if defined(__APPLE__)
#include <TargetConditionals.h> // @manual
#endif

namespace folly::python {

#if defined(__APPLE__) && (TARGET_OS_SIMULATOR || TARGET_OS_IPHONE)
// Apple mobile OSes ship no Python interpreter; the definition below
// requires it. The buffer-copy helpers after it stay available.
#else

namespace {

struct PyBufferData {
  folly::Executor* executor;
  PyObject* py_object;
};

} // namespace

std::unique_ptr<folly::IOBuf> iobuf_from_memoryview(
    folly::Executor* executor,
    PyObject* py_object,
    void* buf,
    uint64_t length) {
  Py_INCREF(py_object);
  auto* pyUserData = new PyBufferData();
  pyUserData->executor = executor;
  pyUserData->py_object = py_object;

  return folly::IOBuf::takeOwnership(
      buf,
      length,
      [](void* /* buf */, void* userData) {
        auto* py_data = (PyBufferData*)userData;
        auto* pyObject = py_data->py_object;
        // First of all make sure python is not finalizing, in that case don't
        // bother taking care of memory the process is about to die anyway.
        if (!Py_IsFinalizing()) {
          if (PyGILState_Check()) {
            // if we have the GIL, we can just decref the object.
            Py_DECREF(pyObject);
          } else if (py_data->executor) {
            // We have an executor, so we can schedule the decref on it.
            // The task must take the GIL itself: deallocating without it
            // is undefined behavior (faults on Windows).
            py_data->executor->add([pyObject]() mutable {
              if (!Py_IsFinalizing()) {
                PyGILState_STATE gil_state = PyGILState_Ensure();
                Py_DECREF(pyObject);
                PyGILState_Release(gil_state);
              }
            });
          } else {
            /*
              This is the last ditch effort. We don't have the GIL and we have
              no asyncio executor.  In this case we will attempt to use the
              pendingCall interface to cpython.  This is likely to fail under
              heavy load due to lock contention.
            */
            int ret = Py_AddPendingCall(
                [](void* userData1) {
                  Py_DECREF((PyObject*)userData1);
                  return 0;
                },
                (void*)pyObject);
            if (ret == -1) {
              // That failed, we are out of options
              // Grab the gil and decref the object
              // This is better than leaking memory
              PyGILState_STATE gil_state = PyGILState_Ensure();
              Py_DECREF(pyObject);
              PyGILState_Release(gil_state);
            }
          }
        }
        delete py_data;
      },
      pyUserData);
}

#endif // Apple mobile OSes ship no Python interpreter

std::unique_ptr<folly::IOBuf> iobuf_copy_from_buffer(
    const void* buf, std::size_t length) {
  // Create an IOBuf that OWNS a copy of the data. Unlike
  // iobuf_from_memoryview (which shares the Python buffer via Py_INCREF),
  // this function copies data into IOBuf-managed memory. Use this when the
  // IOBuf may outlive the Python buffer (e.g., thrift transport retains the
  // IOBuf after the Python caller has dropped its references).
  auto iobuf = folly::IOBuf::create(length);
  std::memcpy(iobuf->writableData(), buf, length);
  iobuf->append(length);
  return iobuf;
}

std::unique_ptr<folly::IOBuf> create_iobuf(std::size_t capacity) {
  return folly::IOBuf::create(capacity);
}

} // namespace folly::python
