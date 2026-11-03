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

#define FOLLY_PYTHON_EXECUTOR_DETAIL_DEFS
#include <folly/python/executor.h>
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

namespace executor_detail {

AsyncioExecutor* (*get_running_executor)(int) = nullptr;
int (*set_executor_for_loop)(PyObject*, AsyncioExecutor*) = nullptr;

void FOLLY_PYTHON_EXECUTOR_API assign_funcs(
    AsyncioExecutor* (*_get_running_executor)(int),
    int (*_set_executor_for_loop)(PyObject*, AsyncioExecutor*)) {
  get_running_executor = _get_running_executor;
  set_executor_for_loop = _set_executor_for_loop;
}

} // namespace executor_detail

int import_folly_executor_impl() {
  // This is exactly what cython does, but in a Weak Friendly Way
  int ret = 0;
  PyObject* mod = nullptr;
  mod = PyImport_ImportModule("folly.executor");
  if (mod == nullptr) { // We failed to import
    ret = -1;
  }
  Py_DecRef(mod);
  return ret;
}

constinit static import_cache import_folly_executor{
    import_folly_executor_impl, "folly.executor"};

FOLLY_PYTHON_EXECUTOR_API folly::Executor* getExecutor() {
  if (!isLinked()) {
    // Python isn't even linked
    return nullptr;
  }
  import_folly_executor();
  DCHECK(executor_detail::get_running_executor != nullptr);
  return executor_detail::get_running_executor(
      false); // TODO: fried set this to true
}

FOLLY_PYTHON_EXECUTOR_API int setExecutorForLoop(
    PyObject* loop, AsyncioExecutor* executor) {
  if (!isLinked()) {
    // Python isn't even linked
    return -2;
  }
  import_folly_executor();
  DCHECK(executor_detail::set_executor_for_loop != nullptr);
  return executor_detail::set_executor_for_loop(loop, executor);
}
#endif // Apple mobile OSes ship no Python interpreter

} // namespace python
} // namespace folly
