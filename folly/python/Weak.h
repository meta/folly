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
// Apple mobile OSes ship no Python interpreter; the header body below
// requires it.
namespace folly::python {
// Without an interpreter there is nothing to link against, and nothing
// that could be finalizing.
inline bool isLinked() {
  return false;
}
inline bool isFinalizing() {
  return false;
}
} // namespace folly::python
#else

#include <Python.h> // @manual=fbsource//third-party/python:python-headers

extern "C" {

// NOT_WINDOWS
#if defined(__APPLE__) || defined(__linux__)

#if defined(__APPLE__)
#define Py_Weak(RTYPE) __attribute__((weak_import)) extern RTYPE
#else
#define Py_Weak(RTYPE) __attribute__((weak)) extern RTYPE
#endif

// These symbols provides our base for detecting python is loaded
// See folly::python::isLinked()
Py_Weak(void) Py_IncRef(PyObject*);
Py_Weak(void) Py_DecRef(PyObject*);
Py_Weak(const char*) Py_GetVersion(void);

// Modules
Py_Weak(PyObject*) PyImport_ImportModule(const char*);

// Exception Handling
Py_Weak(PyObject*) PyErr_Occurred(void);
Py_Weak(void) PyErr_Clear(void);
Py_Weak(void) PyErr_Fetch(PyObject**, PyObject**, PyObject**);

// Object Handling
Py_Weak(PyObject*) PyObject_Repr(PyObject*);
Py_Weak(PyObject*) PyObject_GetAttrString(PyObject*, const char*);

// Unicode && Bytes Handling
Py_Weak(char*) PyBytes_AsString(PyObject*);
Py_Weak(PyObject*)
    PyUnicode_AsEncodedString(PyObject*, const char*, const char*);

Py_Weak(const char*) PyUnicode_AsUTF8(PyObject*);

// Thread Identity
Py_Weak(unsigned long) PyThread_get_thread_ident(void);

// Basic GIL Handling
Py_Weak(PyThreadState*) PyThreadState_Get(void);
#if PY_VERSION_HEX >= 0x030d0000 // >= 3.13
Py_Weak(PyThreadState*) PyThreadState_GetUnchecked(void);
#else
Py_Weak(PyThreadState*) _PyThreadState_UncheckedGet(void);
#endif
Py_Weak(PyThreadState*) PyGILState_GetThisThreadState(void);
Py_Weak(int) PyGILState_Check(void);
Py_Weak(PyGILState_STATE) PyGILState_Ensure(void);
Py_Weak(void) PyGILState_Release(PyGILState_STATE);
Py_Weak(PyThreadState*) PyEval_SaveThread(void);
Py_Weak(void) PyEval_RestoreThread(PyThreadState*);

// Some Frame and Traceback Handling
Py_Weak(PyFrameObject*) PyThreadState_GetFrame(PyThreadState*);
Py_Weak(int) PyFrame_GetLineNumber(PyFrameObject*);
Py_Weak(void) _Py_DumpTraceback(int, PyThreadState*);
Py_Weak(PyCodeObject*) PyFrame_GetCode(PyFrameObject*);
Py_Weak(PyFrameObject*) PyFrame_GetBack(PyFrameObject*);

Py_Weak(PyObject*) PyLong_FromVoidPtr(void*);

#if PY_VERSION_HEX >= 0x030b0000 // >= 3.11
Py_Weak(int) PyFrame_GetLasti(PyFrameObject*);
#endif

// Runtime State
Py_Weak(int) Py_IsInitialized(void);
#if PY_VERSION_HEX >= 0x030d0000 // >= 3.13
Py_Weak(int) Py_IsFinalizing(void);
#else
Py_Weak(int) _Py_IsFinalizing(void);
#endif

// Python Types
Py_Weak(PyTypeObject) PyFrame_Type;
Py_Weak(PyTypeObject) PyFunction_Type;

#undef Py_Weak
#endif // NOT_WINDOWS

} // extern "C"

// Torch had the same idea.
#ifndef PYTHONCAPI_COMPAT
// So windows can use these helpers
#if PY_VERSION_HEX < 0x030b0000 // < 3.11
#include <frameobject.h>
inline int PyFrame_GetLasti(PyFrameObject* frame) {
  return frame->f_lasti;
}
#endif

#if PY_VERSION_HEX < 0x030d0000 // < 3.13
inline int Py_IsFinalizing() {
  return _Py_IsFinalizing();
}
#endif
#endif // PYTHONCAPI_COMPAT

namespace folly::python {

// Lets use these symbols if they are defined we can assume python is loaded
inline bool isLinked() {
#if defined(__APPLE__) || defined(__linux__)
  return (Py_IncRef != nullptr) && (Py_DecRef != nullptr) &&
      (Py_GetVersion != nullptr);
#else
#if defined(_WIN32)
  return true;
#else
  return false;
#endif
#endif
}

// The interpreter may be absent at runtime even where it is available at
// build time; only consult the finalization state when linked.
inline bool isFinalizing() {
  return isLinked() && (Py_IsFinalizing() != 0);
}

} // namespace folly::python

#endif // Apple mobile OSes ship no Python interpreter
