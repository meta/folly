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

#include <span>

#include <folly/Portability.h>
#include <folly/lang/Hint.h>

namespace folly {

/**
 * Call doNotOptimizeAway(var) to ensure that var will be computed even
 * post-optimization.  Use it for variables that are computed during
 * benchmarking but otherwise are useless. The compiler tends to do a
 * good job at eliminating unused variables, and this function fools it
 * into thinking var is in fact needed.
 *
 * Call makeUnpredictable(var) when you don't want the optimizer to use
 * its knowledge of var to shape the following code.  This is useful
 * when constant propagation or power reduction is possible during your
 * benchmark but not in real use cases.
 */

template <class T>
FOLLY_ALWAYS_INLINE void doNotOptimizeAway(const T& datum) {
  compiler_must_not_elide(datum);
}

template <typename T>
FOLLY_ALWAYS_INLINE void makeUnpredictable(T& datum) {
  compiler_must_not_predict(datum);
}

namespace detail {
size_t bm_llc_size_fallback();

/// bm_llc_evict_touch
///
/// Writes the first word of each stride and the last word of the region.
/// words_per_line must be positive.
void bm_llc_evict_touch(
    std::span<size_t volatile> region, size_t words_per_line, size_t value);
} // namespace detail

/// bm_llc_size
///
/// Calculates the size of the LLC (Last Level Cache, typically L3 on x86-64).
size_t bm_llc_size();

/// bm_llc_evict
///
/// Applies cache pressure with a region twice the detected LLC size.
/// Eviction is best effort, not a guarantee that subsequent accesses reach
/// DRAM.
void bm_llc_evict(size_t value);

} // namespace folly
