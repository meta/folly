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

#include <folly/detail/TrapOnAvx512.h>

#include <folly/Portability.h>
#include <folly/portability/GTest.h>

#include <cstdint>

namespace folly::detail {

// This target is not built with BOLT `--trap-avx512`, so it should always
// return false (including on non-x64 platforms).
TEST(TrapOnAvx512TestNegative, Basic) {
  ASSERT_FALSE(hasTrapOnAvx512());
}

TEST(TrapOnAvx512TestNegative, NoFunctionTrappedOnEntry) {
  EXPECT_FALSE(hasTrapOnEntry(reinterpret_cast<const void*>(&hasTrapOnAvx512)));
  EXPECT_FALSE(hasTrapOnEntry(reinterpret_cast<const void*>(&hasTrapOnEntry)));
}

// The bytes come from a BOLTed binary: a trapped entry, an EVEX instruction
// BOLT left alone, and the jump BOLT writes over the old copy of a function
// it moved.
// BOLT traps only x86-64 code, so elsewhere nothing reads as trapped.
TEST(TrapOnAvx512Test, TrapOnEntryReadsTheFirstTwoBytes) {
  static constexpr uint8_t kTrapped[] = {0x0f, 0x0b, 0x66, 0x2e, 0x0f, 0x1f};
  static constexpr uint8_t kEvex[] = {0x62, 0xe2, 0xf5, 0x40, 0x2c, 0xda};
  static constexpr uint8_t kJmp[] = {0xe9, 0x2f, 0xeb, 0x1b, 0x1b, 0xcc};
  static constexpr uint8_t kSwapped[] = {0x0b, 0x0f};
  static constexpr uint8_t kFirstOnly[] = {0x0f, 0x0c};
  EXPECT_EQ(kIsArchAmd64, hasTrapOnEntry(kTrapped));
  EXPECT_FALSE(hasTrapOnEntry(kEvex));
  EXPECT_FALSE(hasTrapOnEntry(kJmp));
  EXPECT_FALSE(hasTrapOnEntry(kSwapped));
  EXPECT_FALSE(hasTrapOnEntry(kFirstOnly));
}

} // namespace folly::detail
