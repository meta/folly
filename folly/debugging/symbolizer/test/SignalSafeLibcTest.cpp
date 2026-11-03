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

#include <folly/debugging/symbolizer/detail/SignalSafeLibc.h>

#include <cerrno>
#include <cstring>

#include <folly/debugging/symbolizer/detail/Debug.h>
#include <folly/portability/GTest.h>

namespace {
namespace libc = folly::symbolizer::detail::signal_safe_libc;
namespace impl = libc::impl;
using folly::symbolizer::detail::get_libc_symbol;

#if FOLLY_HAVE_SWAPCONTEXT
void* contextResolver(const char* name) {
  EXPECT_TRUE(
      std::strcmp(name, "getcontext") == 0 ||
      std::strcmp(name, "makecontext") == 0 ||
      std::strcmp(name, "swapcontext") == 0);
  return get_libc_symbol(name);
}
#endif
} // namespace

TEST(SignalSafeLibc, WarmUpPreservesErrno) {
  const int savedErrno = errno;
  errno = ERANGE;
  libc::warmUp();
  EXPECT_EQ(ERANGE, errno);
  errno = savedErrno;
}

TEST(SignalSafeLibc, WarmUpPreservesSignalMask) {
  sigset_t before;
  sigset_t after;
  ASSERT_EQ(0, sigprocmask(SIG_SETMASK, nullptr, &before));
  libc::warmUp();
  ASSERT_EQ(0, sigprocmask(SIG_SETMASK, nullptr, &after));
  for (int sig = 1; sig < NSIG; ++sig) {
    EXPECT_EQ(sigismember(&before, sig), sigismember(&after, sig));
  }
}

#if FOLLY_HAVE_SWAPCONTEXT
TEST(SignalSafeLibc, RequiresContextFunctionsOnly) {
  const auto functions = impl::loadFunctions(contextResolver);
  ASSERT_TRUE(bool(functions));
  auto missing = functions;
  missing.getcontext = nullptr;
  EXPECT_FALSE(bool(missing));
  missing = functions;
  missing.makecontext = nullptr;
  EXPECT_FALSE(bool(missing));
  missing = functions;
  missing.swapcontext = nullptr;
  EXPECT_FALSE(bool(missing));
}
#endif
