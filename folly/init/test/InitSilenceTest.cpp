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

#include <folly/init/Init.h>

#include <folly/portability/GTest.h>

TEST(Init, printsNothing) {
  int argc = 1;
  char name[] = "init_test";
  char* args[] = {name, nullptr};
  char** argv = args;

  ::testing::internal::CaptureStdout();
  ::testing::internal::CaptureStderr();
  {
    folly::Init init(&argc, &argv);
  }
  EXPECT_EQ("", ::testing::internal::GetCapturedStdout());
  EXPECT_EQ("", ::testing::internal::GetCapturedStderr());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
