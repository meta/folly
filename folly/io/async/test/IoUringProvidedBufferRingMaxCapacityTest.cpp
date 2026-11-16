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

#include <folly/io/async/IoUringProvidedBufferRing.h>

#include <exception>
#include <system_error>

#include <gtest/gtest.h>

#include <folly/ScopeGuard.h>
#include <folly/io/async/IoUringSetupRetry.h>
#include <folly/lang/Exception.h>
#include <folly/testing/TestUtil.h>

#if FOLLY_HAS_LIBURING

namespace folly {
namespace {

TEST(IoUringProvidedBufferRingMaxCapacityTest, Create) {
  io_uring ring{};
  const auto ret = detail::retryTransientIoUringSetup([&] {
    return ::io_uring_queue_init(2, &ring, 0);
  });
  if (ret == -ENOMEM) {
    const std::system_error error(
        ENOMEM, std::generic_category(), "io_uring_queue_init");
    test::report_resource_exhaustion(
        "io_uring setup", error.code(), error.what());
    throw_exception(error);
  }
  ASSERT_EQ(0, ret);
  auto cleanup = makeGuard([&] { ::io_uring_queue_exit(&ring); });

  using RegistrationError = IoUringProvidedBufferRing::LibUringCallError;
  std::exception_ptr registrationError;
  auto rollback = makeGuard([&] {
    if (const auto error = get_exception<RegistrationError>(registrationError);
        error && error->code() == std::errc::not_enough_memory) {
      test::report_resource_exhaustion(
          "io_uring setup", error->code(), error->what());
    }
  });

  IoUringProvidedBufferRing::Options options = {
      .gid = 0,
      .bufferCount = 32768,
      .bufferSize = 32,
  };
  auto maxRing = detail::retryTransientIoUringSetup<std::exception>(
      [&] { return IoUringProvidedBufferRing::create(&ring, options); },
      [&](const std::exception&) {
        registrationError = current_exception();
        const auto error = get_exception<RegistrationError>(registrationError);
        return error && error->code() == std::errc::not_enough_memory;
      });
  rollback.dismiss();

  EXPECT_EQ(maxRing->count(), 32768);
  maxRing.reset();
}

} // namespace
} // namespace folly

#endif
