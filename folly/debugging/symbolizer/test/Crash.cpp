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

#include <signal.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <folly/debugging/symbolizer/SignalHandler.h>

int main() {
  alignas(16) static std::array<char, 1024 * 1024> stack{};
  stack_t ss{};
  ss.ss_sp = stack.data();
  ss.ss_size = stack.size();
  if (sigaltstack(&ss, nullptr) != 0) {
    std::perror("sigaltstack");
    return 1;
  }
  folly::symbolizer::installFatalSignalHandler();
  std::abort();
}
