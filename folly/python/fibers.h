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

/*
 *  This file serves as a helper for bridging folly fibers and python
 *  asyncio.future.
 */

#pragma once

#include <folly/fibers/FiberManagerInternal.h>
#include <folly/python/Weak.h>

#if defined(__APPLE__)
#include <TargetConditionals.h> // @manual
#endif

#ifdef FOLLY_PYTHON_WIN_SHAREDLIB
#ifdef FOLLY_PYTHON_FIBERS_DETAIL_DEFS
#define FOLLY_PYTHON_FIBERS_API __declspec(dllexport)
#else
#define FOLLY_PYTHON_FIBERS_API __declspec(dllimport)
#endif
#else
#define FOLLY_PYTHON_FIBERS_API
#endif

namespace folly {
namespace python {

namespace fibers_detail {
FOLLY_PYTHON_FIBERS_API void assign_func(
    folly::fibers::FiberManager* (*_get_fiber_manager)(
        const folly::fibers::FiberManager::Options&));
} // namespace fibers_detail

// Must be called from main context
FOLLY_PYTHON_FIBERS_API folly::fibers::FiberManager* getFiberManager(
    const folly::fibers::FiberManager::Options& opts = {});

#if defined(__APPLE__) && (TARGET_OS_SIMULATOR || TARGET_OS_IPHONE)
// Apple mobile OSes ship no Python interpreter; the helper below requires
// it.
#else
/**
 * Helper function with similar callback/userData parameters as bridgeFuture.
 * This can be convenient in code that calls both (notably our tests),
 * but most callsites should directly use getFiberManager().
 */
template <typename T>
void bridgeFibers(
    folly::Function<T()>&& function,
    folly::Function<void(folly::Try<T>&&, PyObject*)> callback,
    PyObject* userData) {
  auto* fiberManager = getFiberManager();
  // We are handing over a pointer to a python object to c++ and need
  // to make sure it isn't removed by python in that time.
  Py_IncRef(userData);
  auto guard = folly::makeGuard([=] { Py_DecRef(userData); });
  fiberManager->addTask(
      [function = std::move(function),
       callback = std::move(callback),
       userData,
       guard = std::move(guard)]() mutable {
        // This will run from inside the gil, called by the asyncio add_reader
        auto res = folly::makeTryWith([&] { return function(); });
        callback(std::move(res), userData);
        // guard goes out of scope here, and its stored function is called
      });
}

#endif // Apple mobile OSes ship no Python interpreter

} // namespace python
} // namespace folly
