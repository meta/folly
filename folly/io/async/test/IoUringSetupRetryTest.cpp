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

#include <folly/io/async/IoUringSetupRetry.h>

#include <cerrno>
#include <memory>
#include <stdexcept>
#include <system_error>

#include <gtest/gtest.h>

namespace folly {
namespace {

bool isOutOfMemory(const std::system_error& error) {
  return error.code() == std::errc::not_enough_memory;
}

TEST(IoUringSetupRetryTest, PredicateReturnsWithoutRetry) {
  int attempts = 0;
  int classifications = 0;
  const auto result = detail::retryTransientIoUringSetup<std::system_error>(
      [&] {
        ++attempts;
        return 42;
      },
      [&](const std::system_error&) {
        ++classifications;
        return true;
      });
  EXPECT_EQ(result, 42);
  EXPECT_EQ(attempts, 1);
  EXPECT_EQ(classifications, 0);
}

TEST(IoUringSetupRetryTest, PredicateRetriesEnomem) {
  int attempts = 0;
  int classifications = 0;
  auto result = detail::retryTransientIoUringSetup<std::system_error>(
      [&] {
        if (++attempts < 3) {
          throw std::system_error(ENOMEM, std::generic_category(), "setup");
        }
        return std::make_unique<int>(42);
      },
      [&](const std::system_error& error) {
        ++classifications;
        EXPECT_EQ(error.code(), std::errc::not_enough_memory);
        return isOutOfMemory(error);
      });
  ASSERT_TRUE(result);
  EXPECT_EQ(*result, 42);
  EXPECT_EQ(attempts, 3);
  EXPECT_EQ(classifications, 2);
}

TEST(IoUringSetupRetryTest, PredicateRejectsOtherErrnos) {
  int attempts = 0;
  try {
    detail::retryTransientIoUringSetup<std::system_error>(
        [&]() -> int {
          ++attempts;
          throw std::system_error(EBADF, std::generic_category(), "setup");
        },
        isOutOfMemory);
    FAIL() << "Expected the setup error";
  } catch (const std::system_error& error) {
    EXPECT_EQ(error.code(), std::errc::bad_file_descriptor);
  }
  EXPECT_EQ(attempts, 1);
}

TEST(IoUringSetupRetryTest, PredicatePropagatesOtherExceptionTypes) {
  int attempts = 0;
  int classifications = 0;
  EXPECT_THROW(
      detail::retryTransientIoUringSetup<std::system_error>(
          [&]() -> int {
            ++attempts;
            throw std::runtime_error("setup");
          },
          [&](const std::system_error&) {
            ++classifications;
            return true;
          }),
      std::runtime_error);
  EXPECT_EQ(attempts, 1);
  EXPECT_EQ(classifications, 0);
}

TEST(IoUringSetupRetryTest, ReturnCodeRetriesEnomem) {
  int attempts = 0;
  const auto result = detail::retryTransientIoUringSetup([&] {
    return ++attempts < 3 ? -ENOMEM : 0;
  });
  EXPECT_EQ(result, 0);
  EXPECT_EQ(attempts, 3);
}

TEST(IoUringSetupRetryTest, ReturnCodeRejectsOtherErrnos) {
  int attempts = 0;
  const auto result = detail::retryTransientIoUringSetup([&] {
    ++attempts;
    return -EBADF;
  });
  EXPECT_EQ(result, -EBADF);
  EXPECT_EQ(attempts, 1);
}

TEST(IoUringSetupRetryTest, TypedExceptionRetries) {
  int attempts = 0;
  const auto result =
      detail::retryTransientIoUringSetup<std::runtime_error>([&] {
        if (++attempts < 3) {
          throw std::runtime_error("setup");
        }
        return 42;
      });
  EXPECT_EQ(result, 42);
  EXPECT_EQ(attempts, 3);
}

} // namespace
} // namespace folly
