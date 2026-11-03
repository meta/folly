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

#define FOLLY_PYTHON_FIBERS_DETAIL_DEFS
#include <folly/python/fibers.h>
#include <folly/python/import.h>

#if defined(__APPLE__)
#include <TargetConditionals.h> // @manual
#endif

namespace folly {
namespace python {

#if defined(__APPLE__) && (TARGET_OS_SIMULATOR || TARGET_OS_IPHONE)
// Apple mobile OSes ship no Python interpreter; the definitions below
// require it.
#else

namespace fibers_detail {
// Will be filled in from Cython Side
folly::fibers::FiberManager* (*get_fiber_manager)(
    const folly::fibers::FiberManager::Options&);

FOLLY_PYTHON_FIBERS_API void assign_func(
    folly::fibers::FiberManager* (*_get_fiber_manager)(
        const folly::fibers::FiberManager::Options&)) {
  get_fiber_manager = _get_fiber_manager;
}
} // namespace fibers_detail

int import_folly_fiber_manager_impl() {
  // This is exactly what cython does, but in a Weak Friendly Way
  int ret = 0;
  PyObject* mod = nullptr;
  mod = PyImport_ImportModule("folly.fiber_manager");
  if (mod == nullptr) { // We failed to import
    ret = -1;
  }
  Py_DecRef(mod);
  return ret;
}

constinit static import_cache import_folly_fiber_manager{
    import_folly_fiber_manager_impl, "folly.fiber_manager"};

FOLLY_PYTHON_FIBERS_API folly::fibers::FiberManager* getFiberManager(
    const folly::fibers::FiberManager::Options& opts) {
  DCHECK(!folly::fibers::onFiber());
  if (!isLinked()) {
    // Python isn't even linked
    return nullptr;
  }
  // Python module once loaded will fill-in get_fiber_manager
  import_folly_fiber_manager();
  DCHECK(fibers_detail::get_fiber_manager != nullptr);
  return fibers_detail::get_fiber_manager(opts);
}
#endif // Apple mobile OSes ship no Python interpreter

} // namespace python
} // namespace folly
