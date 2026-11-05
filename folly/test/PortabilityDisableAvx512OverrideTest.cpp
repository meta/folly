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

// Built with -DDISABLE_AVX512 -DFOLLY_DISABLE_AVX512=0: a build rule that sets
// FOLLY_DISABLE_AVX512 itself wins over the mapping from DISABLE_AVX512.

#include <folly/Portability.h>

#include <folly/portability/GTest.h>

TEST(Portability, DisableAvx512SetByBuildRule) {
  EXPECT_EQ(0, FOLLY_DISABLE_AVX512);
}
