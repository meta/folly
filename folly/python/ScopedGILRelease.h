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

#include <folly/python/Weak.h>

#if defined(__APPLE__)
#include <TargetConditionals.h> // @manual
#endif

namespace folly::python {

#if defined(__APPLE__) && (TARGET_OS_SIMULATOR || TARGET_OS_IPHONE)
// Apple mobile OSes ship no Python interpreter; there is no GIL to release.
// The class below requires it.
#else
/**
 * RAII helper to release the Python GIL for the duration of a scope.
 * Only releases if Python is linked, initialized, not finalizing, the current
 * thread has a Python thread state, and the current thread holds the GIL.
 * Safe to use in code that may or may not be called from Python, including
 * non-Python threads in services that embed Python (e.g., C++ services
 * receiving Thrift RPCs that also link Python for subinterpreters or other
 * purposes).
 *
 * Example usage:
 *   void expensiveBlockingOperation() {
 *     ScopedGILRelease gilRelease;
 *     // GIL is released here, allowing other Python threads to run
 *     doBlockingIO();
 *   } // GIL is automatically reacquired when gilRelease goes out of scope
 */
class ScopedGILRelease {
 public:
  ScopedGILRelease()
      : tState_(
            (isLinked() && (Py_IsInitialized() != 0) && !Py_IsFinalizing() &&
             (PyGILState_GetThisThreadState() != nullptr) &&
             (PyGILState_Check() != 0))
                ? PyEval_SaveThread()
                : nullptr) {}

  ~ScopedGILRelease() {
    if (tState_) {
      PyEval_RestoreThread(tState_);
    }
  }

  ScopedGILRelease(const ScopedGILRelease&) = delete;
  ScopedGILRelease& operator=(const ScopedGILRelease&) = delete;
  ScopedGILRelease(ScopedGILRelease&&) = delete;
  ScopedGILRelease& operator=(ScopedGILRelease&&) = delete;

 private:
  PyThreadState* tState_;
};

#endif // Apple mobile OSes ship no Python interpreter

} // namespace folly::python
