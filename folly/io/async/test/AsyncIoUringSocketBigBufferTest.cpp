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

#include <string>

#include <folly/io/async/test/AsyncIoUringSocketTestHelper.h>
#include <folly/portability/GTest.h>

namespace folly {

TEST_P(AsyncIoUringSocketTestAll, WriteBigDrop) {
  MAYBE_SKIP();
  auto [e, s, cb] = makeConnected();
  cb->setHoldData(false); // should trigger overflow in provided buffers
  std::string big(4000000, 'X');
  s->write(&nullWriteCallback, big.c_str(), big.size());
  EXPECT_EQ(big, cb->waitFor(big.size()).via(base.get()).getVia(base.get()));
}

INSTANTIATE_TEST_SUITE_P(
    AsyncIoUringSocketTest,
    AsyncIoUringSocketTestAll,
    ::testing::ValuesIn(mkAllTestParams()),
    [](const ::testing::TestParamInfo<TestParams>& info) {
      return info.param.testName();
    });

} // namespace folly
