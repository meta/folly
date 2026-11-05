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

#include <folly/portability/Filesystem.h>

namespace folly {
namespace fs {

/**
 * Check whether "path" starts with "prefix".
 * That is, if prefix has n path elements, then the first n elements of
 * path must be the same as prefix.
 *
 * There is a special case if prefix ends with a slash:
 * /foo/bar/ is not a prefix of /foo/bar, but both /foo/bar and /foo/bar/
 * are prefixes of /foo/bar/baz.
 */
bool starts_with(const path& p, const path& prefix);

/**
 * If "path" starts with "prefix", return "path" with "prefix" removed.
 * Otherwise, throw filesystem_error.
 */
path remove_prefix(const path& p, const path& prefix);

/**
 * Canonicalize the parent path, leaving the filename (last component)
 * unchanged.  You may use this before creating a file instead of
 * canonical, which requires that the entire path exists.
 */
path canonical_parent(const path& p, const path& basePath = current_path());

/**
 * Get the path to the current executable.
 *
 * Note that this is not reliable and not recommended in general; it may not be
 * implemented on your platform (in which case it will throw), the executable
 * might have been moved or replaced while running, and applications comprising
 * of multiple executables should use some form of configuration system to
 * find the other executables rather than relying on relative paths from one
 * to another.
 *
 * So this should only be used for tests, logging, or other innocuous purposes.
 */
path executable_path();

/**
 * Replace each '%' in model with a random hex digit.
 */
struct unique_path_fn {
  path operator()(path const& model = "%%%%-%%%%-%%%%-%%%%") const;
};
inline constexpr unique_path_fn unique_path;

using std_fs_unique_path_fn = unique_path_fn;
inline constexpr std_fs_unique_path_fn std_fs_unique_path;

} // namespace fs
} // namespace folly
