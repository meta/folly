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

#include <folly/coro/WithMetadata.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include <folly/coro/GtestHelpers.h>
#include <folly/coro/Task.h>
#include <folly/coro/detail/CurrentAsyncFrame.h>
#include <folly/portability/GTest.h>
#include <folly/tracing/AsyncStack.h>

#if FOLLY_HAS_COROUTINES && FOLLY_HAS_ASYNC_STACK_METADATA

namespace {

using namespace folly;
using namespace folly::coro;

struct CurrentScope {
  bool hasWrapper;
  bool hasMarker;
  AsyncStackMetadata metadata;
};

Task<CurrentScope> inspectCurrentScope(const AsyncStackFrame& expectedParent) {
  // `co_withMetadata()` builds `child -> wrapper -> marker -> parent`. The
  // marker labels the child's saved return address, which resumes the wrapper.
  auto& childFrame = co_await coro::detail::co_current_async_stack_frame;
  const auto& wrapperFrame = *childFrame.getParentFrame();
  const auto& metadataFrame = *wrapperFrame.getParentFrame();
  // `hasWrapper` means the child reaches its expected parent through one extra
  // frame.
  co_return CurrentScope{
      .hasWrapper = &wrapperFrame != &expectedParent &&
          metadataFrame.getParentFrame() == &expectedParent,
      .hasMarker = folly::detail::isAsyncStackMetadataFrame(metadataFrame),
      .metadata = folly::detail::getAsyncStackTraceEntryMetadata(childFrame)};
}

CO_TEST(WithMetadata, ZeroMetadataAddsMarker) {
  auto& parentFrame = co_await coro::detail::co_current_async_stack_frame;
  const auto scope =
      co_await co_withMetadata(0, inspectCurrentScope(parentFrame));
  EXPECT_TRUE(scope.hasWrapper);
  EXPECT_TRUE(scope.hasMarker);
  EXPECT_EQ(scope.metadata, 0);
}

template <typename SemiAwaitable>
using WithMetadataResult =
    decltype(co_withMetadata(std::uintptr_t{}, FOLLY_DECLVAL(SemiAwaitable)));

static_assert(std::is_same_v<WithMetadataResult<Task<int>>, Task<int>>);
static_assert(
    std::is_same_v<WithMetadataResult<TaskWithExecutor<int>>, Task<int>>);
static_assert(std::is_same_v<WithMetadataResult<Task<int&>>, Task<int&>>);

CO_TEST(WithMetadata, PreservesTaskWrapperProperties) {
  { // `now_task` remains immediately awaitable.
    static_assert(
        std::is_same_v<WithMetadataResult<now_task<int>>, now_task<int>>);
    static_assert(
        std::is_same_v<
            WithMetadataResult<now_task_with_executor<int>>,
            now_task<int>>);

    auto& parentFrame = co_await coro::detail::co_current_async_stack_frame;
    const auto scope = co_await co_withMetadata(
        17, to_now_task(inspectCurrentScope(parentFrame)));
    EXPECT_EQ(scope.metadata, AsyncStackMetadata{17});
  }
  { // `value_task` retains its safe-alias level.
    static_assert(
        std::is_same_v<WithMetadataResult<value_task<int>>, value_task<int>>);
    static_assert(
        std::is_same_v<
            WithMetadataResult<
                safe_task_with_executor<safe_alias::maybe_value, int>>,
            value_task<int>>);

    auto readMetadata = []() -> value_task<AsyncStackMetadata> {
      auto& frame = co_await coro::detail::co_current_async_stack_frame;
      co_return folly::detail::getAsyncStackTraceEntryMetadata(frame);
    };
    const auto metadata = co_await co_withMetadata(29, readMetadata());
    EXPECT_EQ(metadata, AsyncStackMetadata{29});
  }
}

CO_TEST(WithMetadata, MoveOnlyResult) {
  auto child = []() -> Task<std::unique_ptr<int>> {
    co_return std::make_unique<int>(42);
  };

  auto value = co_await co_withMetadata(111, child());
  EXPECT_EQ(*value, 42);
}

CO_TEST(WithMetadata, ReferenceResult) {
  int value = 42;
  auto child = [&]() -> Task<int&> { co_return value; };

  auto&& result = co_await co_withMetadata(111, child());
  EXPECT_EQ(&result, &value);
}

CO_TEST(WithMetadata, PropagatesException) {
  auto child = []() -> Task<> {
    throw std::runtime_error("test failure");
    co_return;
  };

  CO_ASSERT_THROW(co_await co_withMetadata(111, child()), std::runtime_error);
}

} // namespace

#endif // FOLLY_HAS_COROUTINES && FOLLY_HAS_ASYNC_STACK_METADATA
