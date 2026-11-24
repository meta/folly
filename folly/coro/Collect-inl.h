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

#include <atomic>
#include <type_traits>
#include <utility>

#include <folly/CancellationToken.h>
#include <folly/ExceptionWrapper.h>
#include <folly/coro/AsyncPipe.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/Mutex.h>
#include <folly/coro/detail/Barrier.h>
#include <folly/coro/detail/BarrierTask.h>
#include <folly/coro/detail/CurrentAsyncFrame.h>
#include <folly/coro/detail/Helpers.h>

#if FOLLY_HAS_COROUTINES

namespace folly {
namespace coro {
namespace detail {

template <typename T>
T&& getValueOrUnit(Try<T>&& value) {
  assert(value.hasValue());
  return std::move(value).value();
}

inline Unit getValueOrUnit([[maybe_unused]] Try<void>&& value) {
  assert(value.hasValue());
  return Unit{};
}

template <
    typename InputRange,
    typename Make,
    typename Iter = invoke_result_t<access::begin_fn, InputRange&>,
    typename Elem = std::remove_cvref_t<decltype(*std::declval<Iter&>())>,
    typename RTask = invoke_result_t<Make&, Elem, std::size_t>>
std::vector<RTask> collectMakeInnerTaskVec(InputRange& awaitables, Make& make) {
  std::vector<RTask> tasks;

  auto abegin = access::begin(awaitables);
  auto aend = access::end(awaitables);

  if constexpr (std::is_invocable_v<folly::access::size_fn, InputRange&>) {
    tasks.reserve(static_cast<std::size_t>(folly::access::size(awaitables)));
  } else if constexpr (range_has_known_distance_v<InputRange&>) {
    tasks.reserve(static_cast<std::size_t>(std::distance(abegin, aend)));
  }

  std::size_t index = 0;
  for (auto aiter = abegin; aiter != aend; ++aiter) {
    tasks.push_back(make(std::move(*aiter), index++));
  }

  return tasks;
}

namespace collect_detail {
template <typename T>
inline constexpr bool can_co_awaitTry_v =
    is_awaitable_try<semi_await_awaitable_t<T>>;
} // namespace collect_detail

template <typename SemiAwaitableMover, typename Result>
BarrierTask makeCollectAllTryTask(
    Executor::KeepAlive<> executor,
    const CancellationToken& cancelToken,
    SemiAwaitableMover&& mover,
    Try<Result>& result) {
  try {
    using awaitable_type = decltype(static_cast<SemiAwaitableMover&&>(mover)());
    if constexpr (collect_detail::can_co_awaitTry_v<awaitable_type>) {
      result = co_await co_awaitTry(co_viaIfAsync(
          std::move(executor),
          co_withCancellation(
              cancelToken, static_cast<SemiAwaitableMover&&>(mover)())));
    } else if constexpr (std::is_void_v<Result>) {
      co_await co_viaIfAsync(
          std::move(executor),
          co_withCancellation(
              cancelToken, static_cast<SemiAwaitableMover&&>(mover)()));
      result.emplace();
    } else {
      result.emplace(
          co_await co_viaIfAsync(
              std::move(executor),
              co_withCancellation(
                  cancelToken, static_cast<SemiAwaitableMover&&>(mover)())));
    }
  } catch (...) {
    result.emplaceException(current_exception());
  }
}

template <
    typename Ret,
    typename... SemiAwaitables,
    size_t... Indices,
    typename... SemiAwaitablesMovers>
Ret collectAllTryImpl(
    tag_t<Ret, SemiAwaitables...>,
    std::index_sequence<Indices...>,
    SemiAwaitablesMovers... movers) {
  static_assert(sizeof...(Indices) == sizeof...(SemiAwaitables));
  static_assert(sizeof...(Indices) == sizeof...(SemiAwaitablesMovers));
  if constexpr (sizeof...(SemiAwaitables) == 0) {
    co_return std::tuple<>{};
  } else {
    const Executor::KeepAlive<> executor = co_await co_current_executor;
    const CancellationToken& cancelToken =
        co_await co_current_cancellation_token;

    std::tuple<collect_all_try_component_t<SemiAwaitables>...> results;

    folly::coro::detail::BarrierTask tasks[sizeof...(SemiAwaitables)] = {
        makeCollectAllTryTask(
            executor.get_alias(),
            cancelToken,
            static_cast<SemiAwaitablesMovers&&>(movers),
            std::get<Indices>(results))...,
    };

    folly::coro::detail::Barrier barrier{sizeof...(SemiAwaitables) + 1};

    auto& asyncFrame = co_await detail::co_current_async_stack_frame;

    // Use std::initializer_list to ensure that the sub-tasks are launched
    // in the order they appear in the parameter pack.

    // Save the initial context and restore it after starting each task
    // as the task may have modified the context before suspending and we
    // want to make sure the next task is started with the same initial
    // context.
    const auto context = RequestContext::saveContext();
    (void)std::initializer_list<int>{(
        tasks[Indices].start(&barrier, asyncFrame),
        RequestContext::setContext(context),
        0)...};

    // Wait for all of the sub-tasks to finish execution.
    // Should be safe to avoid an executor transition here even if the
    // operation completes asynchronously since all of the child tasks
    // should already have transitioned to the correct executor due to
    // the use of co_viaIfAsync() within makeCollectAllTryTask().
    co_await UnsafeResumeInlineSemiAwaitable{barrier.arriveAndWait()};

    co_return results;
  }
}

template <
    typename Ret,
    typename... SemiAwaitables,
    size_t... Indices,
    typename... SemiFns>
Ret collectAllImpl(
    tag_t<Ret, SemiAwaitables...>,
    std::index_sequence<Indices...>,
    // `semiFns()` is the immovable, must-use-immediately `SemiAwaitable`
    SemiFns... semiFns) {
  if constexpr (sizeof...(SemiAwaitables) == 0) {
    co_return std::tuple<>{};
  } else {
    const Executor::KeepAlive<> executor = co_await co_current_executor;
    const CancellationToken& parentCancelToken =
        co_await co_current_cancellation_token;

    const CancellationSource cancelSource;
    const CancellationToken cancelToken =
        cancellation_token_merge(parentCancelToken, cancelSource.getToken());

    exception_wrapper firstException;

    auto makeTask = [&](auto&& fn, auto& result) -> BarrierTask {
      using await_result =
          semi_await_result_t<decltype(static_cast<decltype(fn)>(fn)())>;
      try {
        if constexpr (std::is_void_v<await_result>) {
          co_await co_viaIfAsync(
              executor.get_alias(),
              co_withCancellation(
                  cancelToken, static_cast<decltype(fn)>(fn)()));
          result.emplace();
        } else {
          result.emplace(
              co_await co_viaIfAsync(
                  executor.get_alias(),
                  co_withCancellation(
                      cancelToken, static_cast<decltype(fn)>(fn)())));
        }
      } catch (...) {
        if (!cancelSource.requestCancellation()) {
          // This was the first failure, remember its error.
          firstException = exception_wrapper{current_exception()};
        }
      }
    };

    std::tuple<collect_all_try_component_t<SemiAwaitables>...> results;

    folly::coro::detail::BarrierTask tasks[sizeof...(SemiAwaitables)] = {
        makeTask(
            static_cast<SemiFns&&>(semiFns), std::get<Indices>(results))...,
    };

    folly::coro::detail::Barrier barrier{sizeof...(SemiAwaitables) + 1};

    // Save the initial context and restore it after starting each task
    // as the task may have modified the context before suspending and we
    // want to make sure the next task is started with the same initial
    // context.
    const auto context = RequestContext::saveContext();

    auto& asyncFrame = co_await detail::co_current_async_stack_frame;

    // Use std::initializer_list to ensure that the sub-tasks are launched
    // in the order they appear in the parameter pack.
    (void)std::initializer_list<int>{(
        tasks[Indices].start(&barrier, asyncFrame),
        RequestContext::setContext(context),
        0)...};

    // Wait for all of the sub-tasks to finish execution.
    // Should be safe to avoid an executor transition here even if the
    // operation completes asynchronously since all of the child tasks
    // should already have transitioned to the correct executor due to
    // the use of co_viaIfAsync() within makeBarrierTask().
    co_await UnsafeResumeInlineSemiAwaitable{barrier.arriveAndWait()};

    if (firstException) {
      co_yield co_error(std::move(firstException));
    }

    co_return std::tuple<collect_all_component_t<SemiAwaitables>...>{
        getValueOrUnit(std::get<Indices>(std::move(results)))...};
  }
}

template <typename InputRange, typename IsTry, typename AsyncScope>
auto makeUnorderedAsyncGeneratorImpl(
    AsyncScope& scope, InputRange awaitables, IsTry) {
  using Item =
      async_generator_from_awaitable_range_item_t<InputRange, IsTry::value>;
  return [](AsyncScope& scopeParam,
            InputRange awaitablesParam) -> AsyncGenerator<Item&&> {
    auto [results, pipe] = AsyncPipe<Item, false>::create();
    struct SharedState {
      explicit SharedState(AsyncPipe<Item, false>&& p) : pipe(std::move(p)) {}

      AsyncPipe<Item, false> pipe;
      const CancellationSource cancelSource;
    };
    auto sharedState = std::make_shared<SharedState>(std::move(pipe));
    auto cancelToken = sharedState->cancelSource.getToken();

    auto guard = folly::makeGuard([&] {
      sharedState->cancelSource.requestCancellation();
    });
    auto ex = co_await co_current_executor;
    size_t expected = 0;
    // Save the initial context and restore it after starting each task
    // as the task may have modified the context before suspending and we
    // want to make sure the next task is started with the same initial
    // context.
    const auto context = RequestContext::saveContext();

    for (auto&& semiAwaitable : static_cast<InputRange&&>(awaitablesParam)) {
      auto task = [](auto semiAwaitableParam, auto state) -> Task<void> {
        auto result = co_await co_awaitTry(std::move(semiAwaitableParam));
        if (!result.hasValue() && !IsTry::value) {
          state->cancelSource.requestCancellation();
        }
        state->pipe.write(std::move(result));
      }(static_cast<decltype(semiAwaitable)&&>(semiAwaitable), sharedState);
      if constexpr (std::is_same_v<AsyncScope, folly::coro::AsyncScope>) {
        scopeParam.add(co_withExecutor(
            ex, co_withCancellation(cancelToken, std::move(task))));
      } else {
        static_assert(std::is_same_v<AsyncScope, CancellableAsyncScope>);
        scopeParam.add(co_withExecutor(ex, std::move(task)), cancelToken);
      }
      ++expected;
      RequestContext::setContext(context);
    }

    while (expected > 0) {
      CancellationCallback cancelCallback(
          co_await co_current_cancellation_token,
          [&]() noexcept { sharedState->cancelSource.requestCancellation(); });

      if constexpr (!IsTry::value) {
        auto result = co_await co_awaitTry(results.next());
        if (result.hasValue() && result->has_value()) {
          co_yield std::move(**result);
          if (--expected) {
            continue;
          }
          result.emplace(); // completion result
        }
        guard.dismiss();
        co_yield co_result(std::move(result));
      } else {
        // Prevent AsyncPipe from receiving cancellation so we get the right
        // number of OperationCancelled.
        auto result = co_await co_withCancellation({}, results.next());
        co_yield std::move(*result);
        if (--expected == 0) {
          guard.dismiss();
          co_return;
        }
      }
    }
  }(scope, std::move(awaitables));
}

template <typename... SemiAwaitables, size_t... Indices>
auto collectAnyImpl(
    std::index_sequence<Indices...>, SemiAwaitables&&... awaitables)
    -> folly::coro::Task<std::pair<
        std::size_t,
        folly::Try<collect_any_component_t<SemiAwaitables...>>>> {
  const CancellationToken& parentCancelToken =
      co_await co_current_cancellation_token;
  const CancellationSource cancelSource;
  const CancellationToken cancelToken =
      cancellation_token_merge(parentCancelToken, cancelSource.getToken());

  std::pair<std::size_t, folly::Try<collect_any_component_t<SemiAwaitables...>>>
      firstCompletion;
  firstCompletion.first = size_t(-1);
  co_await folly::coro::collectAll(
      folly::coro::co_withCancellation(
          cancelToken,
          folly::coro::co_invoke(
              [&, aw = static_cast<SemiAwaitables&&>(awaitables)]() mutable
                  -> folly::coro::Task<void> {
                auto result = co_await folly::coro::co_awaitTry(
                    static_cast<SemiAwaitables&&>(aw));
                if (!cancelSource.requestCancellation()) {
                  // This is first entity to request cancellation.
                  firstCompletion.first = Indices;
                  firstCompletion.second = std::move(result);
                }
              }))...);

  co_return firstCompletion;
}

template <typename... SemiAwaitables, size_t... Indices>
auto collectAnyWithoutExceptionImpl(
    std::index_sequence<Indices...>, SemiAwaitables&&... awaitables)
    -> folly::coro::Task<std::pair<
        std::size_t,
        folly::Try<detail::collect_any_component_t<SemiAwaitables...>>>> {
  const CancellationToken& parentCancelToken =
      co_await co_current_cancellation_token;
  const CancellationSource cancelSource;
  const CancellationToken cancelToken =
      cancellation_token_merge(parentCancelToken, cancelSource.getToken());

  constexpr std::size_t nAwaitables = sizeof...(SemiAwaitables);
  std::atomic<std::size_t> nAwaited = 1;
  std::pair<std::size_t, folly::Try<collect_any_component_t<SemiAwaitables...>>>
      firstValueOrLastException;
  firstValueOrLastException.first = std::numeric_limits<size_t>::max();
  co_await folly::coro::collectAll(
      folly::coro::co_withCancellation(
          cancelToken, [&]() -> folly::coro::Task<void> {
            auto result = co_await folly::coro::co_awaitTry(
                std::forward<SemiAwaitables>(awaitables));
            if ((result.hasValue() ||
                 nAwaited.fetch_add(1, std::memory_order_relaxed) ==
                     nAwaitables) &&
                !cancelSource.requestCancellation()) {
              firstValueOrLastException.first = Indices;
              firstValueOrLastException.second = std::move(result);
            }
          }())...);

  co_return firstValueOrLastException;
}

template <typename... SemiAwaitables, size_t... Indices>
auto collectAnyNoDiscardImpl(
    std::index_sequence<Indices...>, SemiAwaitables&&... awaitables)
    -> folly::coro::Task<
        std::tuple<collect_all_try_component_t<SemiAwaitables>...>> {
  const CancellationSource cancelSource;
  const CancellationToken cancelToken = cancellation_token_merge(
      co_await co_current_cancellation_token, cancelSource.getToken());

  std::tuple<collect_all_try_component_t<SemiAwaitables>...> results;
  co_await folly::coro::collectAll(
      folly::coro::co_withCancellation(
          cancelToken, folly::coro::co_invoke([&]() -> folly::coro::Task<void> {
            auto result = co_await folly::coro::co_awaitTry(
                std::forward<SemiAwaitables>(awaitables));
            cancelSource.requestCancellation();
            std::get<Indices>(results) = std::move(result);
          }))...);

  co_return results;
}

} // namespace detail

template <typename... SemiAwaitables>
auto collectAll(SemiAwaitables... awaitables)
    -> detail::CollectAllTask<SemiAwaitables...> {
  return detail::collectAllImpl(
      tag<detail::CollectAllTask<SemiAwaitables...>, SemiAwaitables...>,
      std::make_index_sequence<sizeof...(SemiAwaitables)>{},
      folly::ext::must_use_immediately_unsafe_mover(
          static_cast<SemiAwaitables&&>(awaitables))...);
}

template <typename... SemiAwaitables>
auto collectAllTry(SemiAwaitables... awaitables)
    -> detail::CollectAllTryTask<SemiAwaitables...> {
  return detail::collectAllTryImpl(
      tag<detail::CollectAllTryTask<SemiAwaitables...>, SemiAwaitables...>,
      std::make_index_sequence<sizeof...(SemiAwaitables)>{},
      folly::ext::must_use_immediately_unsafe_mover(
          static_cast<SemiAwaitables&&>(awaitables))...);
}

template <typename InputRange>
  requires(!std::is_void_v<
           semi_await_result_t<detail::range_reference_t<InputRange>>>)
auto collectAllRange(InputRange awaitables)
    -> folly::coro::Task<std::vector<detail::collect_all_range_component_t<
        detail::range_reference_t<InputRange>>>> {
  const folly::Executor::KeepAlive<> executor = co_await co_current_executor;
  const CancellationSource cancelSource;
  const CancellationToken cancelToken = cancellation_token_merge(
      co_await co_current_cancellation_token, cancelSource.getToken());

  std::vector<detail::collect_all_try_range_component_t<
      detail::range_reference_t<InputRange>>>
      tryResults;

  exception_wrapper firstException;

  using awaitable_type =
      std::remove_cvref_t<detail::range_reference_t<InputRange>>;
  auto makeTask = [&](awaitable_type semiAwaitable, std::size_t index)
      -> detail::BarrierTask {
    assert(index < tryResults.size());

    try {
      tryResults[index].emplace(
          co_await co_viaIfAsync(
              executor.get_alias(),
              co_withCancellation(cancelToken, std::move(semiAwaitable))));
    } catch (...) {
      if (!cancelSource.requestCancellation()) {
        firstException = exception_wrapper{current_exception()};
      }
    }
  };

  auto tasks = detail::collectMakeInnerTaskVec(awaitables, makeTask);

  tryResults.resize(tasks.size());

  // Save the initial context and restore it after starting each task
  // as the task may have modified the context before suspending and we
  // want to make sure the next task is started with the same initial
  // context.
  const auto context = RequestContext::saveContext();

  auto& asyncFrame = co_await detail::co_current_async_stack_frame;

  // Launch the tasks and wait for them all to finish.
  {
    detail::Barrier barrier{tasks.size() + 1};
    for (auto&& task : tasks) {
      task.start(&barrier, asyncFrame);
      RequestContext::setContext(context);
    }
    co_await detail::UnsafeResumeInlineSemiAwaitable{barrier.arriveAndWait()};
  }

  // Check if there were any exceptions and rethrow the first one.
  if (firstException) {
    co_yield co_error(std::move(firstException));
  }

  std::vector<detail::collect_all_range_component_t<
      detail::range_reference_t<InputRange>>>
      results;
  results.reserve(tryResults.size());
  for (auto& result : tryResults) {
    results.emplace_back(std::move(result).value());
  }

  co_return results;
}

template <typename InputRange>
  requires std::is_void_v<
      semi_await_result_t<detail::range_reference_t<InputRange>>>
auto collectAllRange(InputRange awaitables) -> folly::coro::Task<void> {
  const folly::Executor::KeepAlive<> executor = co_await co_current_executor;
  const CancellationSource cancelSource;
  const CancellationToken cancelToken = cancellation_token_merge(
      co_await co_current_cancellation_token, cancelSource.getToken());

  exception_wrapper firstException;

  using awaitable_type =
      std::remove_cvref_t<detail::range_reference_t<InputRange>>;
  auto makeTask =
      [&](awaitable_type semiAwaitable, std::size_t) -> detail::BarrierTask {
    try {
      co_await co_viaIfAsync(
          executor.get_alias(),
          co_withCancellation(cancelToken, std::move(semiAwaitable)));
    } catch (...) {
      if (!cancelSource.requestCancellation()) {
        firstException = exception_wrapper{current_exception()};
      }
    }
  };

  auto tasks = detail::collectMakeInnerTaskVec(awaitables, makeTask);

  // Save the initial context and restore it after starting each task
  // as the task may have modified the context before suspending and we
  // want to make sure the next task is started with the same initial
  // context.
  const auto context = RequestContext::saveContext();

  auto& asyncFrame = co_await detail::co_current_async_stack_frame;

  // Launch the tasks and wait for them all to finish.
  {
    detail::Barrier barrier{tasks.size() + 1};
    for (auto&& task : tasks) {
      task.start(&barrier, asyncFrame);
      RequestContext::setContext(context);
    }
    co_await detail::UnsafeResumeInlineSemiAwaitable{barrier.arriveAndWait()};
  }

  // Check if there were any exceptions and rethrow the first one.
  if (firstException) {
    co_yield co_error(std::move(firstException));
  }
}

template <typename InputRange>
auto collectAllTryRange(InputRange awaitables)
    -> folly::coro::Task<std::vector<detail::collect_all_try_range_component_t<
        detail::range_reference_t<InputRange>>>> {
  std::vector<detail::collect_all_try_range_component_t<
      detail::range_reference_t<InputRange>>>
      results;

  const folly::Executor::KeepAlive<> executor =
      folly::getKeepAliveToken(co_await co_current_executor);

  const CancellationToken& cancelToken = co_await co_current_cancellation_token;

  using awaitable_type =
      std::remove_cvref_t<detail::range_reference_t<InputRange>>;
  auto makeTask = [&](awaitable_type semiAwaitable, std::size_t index)
      -> detail::BarrierTask {
    assert(index < results.size());
    auto& result = results[index];
    if constexpr (detail::collect_detail::can_co_awaitTry_v<awaitable_type>) {
      // Fast path: use co_awaitTry to avoid exception rethrow overhead
      result = co_await co_awaitTry(co_viaIfAsync(
          executor.get_alias(),
          co_withCancellation(cancelToken, std::move(semiAwaitable))));
    } else {
      // Fallback: use try-catch for awaitables without co_awaitTry support
      try {
        using await_result = semi_await_result_t<awaitable_type>;
        if constexpr (std::is_void_v<await_result>) {
          co_await co_viaIfAsync(
              executor.get_alias(),
              co_withCancellation(cancelToken, std::move(semiAwaitable)));
          result.emplace();
        } else {
          result.emplace(
              co_await co_viaIfAsync(
                  executor.get_alias(),
                  co_withCancellation(cancelToken, std::move(semiAwaitable))));
        }
      } catch (...) {
        result.emplaceException(current_exception());
      }
    }
  };

  auto tasks = detail::collectMakeInnerTaskVec(awaitables, makeTask);

  // Now that we know how many tasks there are, allocate that
  // many Try objects to store the results before we start
  // executing the tasks.
  results.resize(tasks.size());

  // Save the initial context and restore it after starting each task
  // as the task may have modified the context before suspending and we
  // want to make sure the next task is started with the same initial
  // context.
  const auto context = RequestContext::saveContext();

  auto& asyncFrame = co_await detail::co_current_async_stack_frame;

  // Launch the tasks and wait for them all to finish.
  {
    detail::Barrier barrier{tasks.size() + 1};
    for (auto&& task : tasks) {
      task.start(&barrier, asyncFrame);
      RequestContext::setContext(context);
    }
    co_await detail::UnsafeResumeInlineSemiAwaitable{barrier.arriveAndWait()};
  }

  co_return results;
}

template <typename InputRange>
  requires std::is_void_v<
      semi_await_result_t<detail::range_reference_t<InputRange>>>
auto collectAllWindowed(InputRange awaitables, std::size_t maxConcurrency)
    -> folly::coro::Task<void> {
  assert(maxConcurrency > 0);

  const folly::Executor::KeepAlive<> executor = co_await co_current_executor;
  const CancellationSource cancelSource;
  const CancellationToken cancelToken = cancellation_token_merge(
      co_await co_current_cancellation_token, cancelSource.getToken());

  exception_wrapper firstException;

  const auto trySetFirstException = [&](exception_wrapper&& e) noexcept {
    if (!cancelSource.requestCancellation()) {
      // This is first entity to request cancellation.
      firstException = std::move(e);
    }
  };

  auto iter = access::begin(awaitables);
  const auto iterEnd = access::end(awaitables);

  using iterator_t = decltype(iter);
  using awaitable_t = typename std::iterator_traits<iterator_t>::value_type;

  folly::coro::Mutex mutex;

  exception_wrapper iterationException;

  auto makeWorker = [&]() -> detail::BarrierTask {
    auto lock =
        co_await co_viaIfAsync(executor.get_alias(), mutex.co_scoped_lock());

    while (!iterationException && iter != iterEnd) {
      std::optional<awaitable_t> awaitable;
      try {
        awaitable.emplace(*iter);
        ++iter;
      } catch (...) {
        iterationException = exception_wrapper{current_exception()};
        cancelSource.requestCancellation();
      }

      if (!awaitable) {
        co_return;
      }

      lock.unlock();

      try {
        co_await co_viaIfAsync(
            executor.get_alias(),
            co_withCancellation(cancelToken, std::move(*awaitable)));
      } catch (...) {
        trySetFirstException(exception_wrapper{current_exception()});
      }

      lock =
          co_await co_viaIfAsync(executor.get_alias(), mutex.co_scoped_lock());
    }
  };

  std::vector<detail::BarrierTask> workerTasks;

  detail::Barrier barrier{1};

  // Save the initial context and restore it after starting each task
  // as the task may have modified the context before suspending and we
  // want to make sure the next task is started with the same initial
  // context.
  const auto context = RequestContext::saveContext();

  auto& asyncFrame = co_await detail::co_current_async_stack_frame;

  try {
    auto lock = co_await mutex.co_scoped_lock();

    while (!iterationException && iter != iterEnd &&
           workerTasks.size() < maxConcurrency) {
      // Unlock the mutex before starting the worker so that
      // it can consume as many results synchronously as it can before
      // returning here and letting us spawn another task.
      // This can avoid spawning more worker coroutines than is necessary
      // to consume all of the awaitables.
      lock.unlock();

      workerTasks.push_back(makeWorker());
      barrier.add(1);
      workerTasks.back().start(&barrier, asyncFrame);

      RequestContext::setContext(context);

      lock = co_await mutex.co_scoped_lock();
    }
  } catch (...) {
    if (workerTasks.empty()) {
      iterationException = exception_wrapper{current_exception()};
    }
  }

  co_await detail::UnsafeResumeInlineSemiAwaitable{barrier.arriveAndWait()};

  if (auto& ex = iterationException ? iterationException : firstException) {
    co_yield co_error(std::move(ex));
  }
}

template <typename InputRange>
  requires(!std::is_void_v<
           semi_await_result_t<detail::range_reference_t<InputRange>>>)
auto collectAllWindowed(InputRange awaitables, std::size_t maxConcurrency)
    -> folly::coro::Task<std::vector<detail::collect_all_range_component_t<
        detail::range_reference_t<InputRange>>>> {
  assert(maxConcurrency > 0);

  const folly::Executor::KeepAlive<> executor = co_await co_current_executor;

  const CancellationToken& parentCancelToken =
      co_await co_current_cancellation_token;
  const CancellationSource cancelSource;
  const CancellationToken cancelToken =
      cancellation_token_merge(parentCancelToken, cancelSource.getToken());

  exception_wrapper firstException;

  auto trySetFirstException = [&](exception_wrapper&& e) noexcept {
    if (!cancelSource.requestCancellation()) {
      // This is first entity to request cancellation.
      firstException = std::move(e);
    }
  };

  auto iter = access::begin(awaitables);
  const auto iterEnd = access::end(awaitables);

  using iterator_t = decltype(iter);
  using awaitable_t = typename std::iterator_traits<iterator_t>::value_type;

  folly::coro::Mutex mutex;

  std::vector<detail::collect_all_try_range_component_t<
      detail::range_reference_t<InputRange>>>
      tryResults;

  if constexpr (std::is_invocable_v<folly::access::size_fn, InputRange&>) {
    tryResults.reserve(
        static_cast<std::size_t>(folly::access::size(awaitables)));
  } else if constexpr (range_has_known_distance_v<InputRange&>) {
    tryResults.reserve(static_cast<std::size_t>(std::distance(iter, iterEnd)));
  }

  exception_wrapper iterationException;

  auto makeWorker = [&]() -> detail::BarrierTask {
    auto lock =
        co_await co_viaIfAsync(executor.get_alias(), mutex.co_scoped_lock());

    while (!iterationException && iter != iterEnd) {
      const std::size_t thisIndex = tryResults.size();
      std::optional<awaitable_t> awaitable;
      try {
        tryResults.emplace_back();
        awaitable.emplace(*iter);
        ++iter;
      } catch (...) {
        iterationException = exception_wrapper{current_exception()};
        cancelSource.requestCancellation();
      }

      if (!awaitable) {
        co_return;
      }

      lock.unlock();

      detail::collect_all_try_range_component_t<
          detail::range_reference_t<InputRange>>
          tryResult;

      try {
        tryResult.emplace(
            co_await co_viaIfAsync(
                executor.get_alias(),
                co_withCancellation(
                    cancelToken, static_cast<awaitable_t&&>(*awaitable))));
      } catch (...) {
        trySetFirstException(exception_wrapper{current_exception()});
      }

      lock =
          co_await co_viaIfAsync(executor.get_alias(), mutex.co_scoped_lock());

      try {
        tryResults[thisIndex] = std::move(tryResult);
      } catch (...) {
        trySetFirstException(exception_wrapper{current_exception()});
      }
    }
  };

  std::vector<detail::BarrierTask> workerTasks;

  detail::Barrier barrier{1};

  exception_wrapper workerCreationException;

  // Save the initial context and restore it after starting each task
  // as the task may have modified the context before suspending and we
  // want to make sure the next task is started with the same initial
  // context.
  const auto context = RequestContext::saveContext();

  auto& asyncFrame = co_await detail::co_current_async_stack_frame;

  try {
    auto lock = co_await mutex.co_scoped_lock();

    while (!iterationException && iter != iterEnd &&
           workerTasks.size() < maxConcurrency) {
      // Unlock the mutex before starting the worker so that
      // it can consume as many results synchronously as it can before
      // returning here and letting us spawn another task.
      // This can avoid spawning more worker coroutines than is necessary
      // to consume all of the awaitables.
      lock.unlock();

      workerTasks.push_back(makeWorker());
      barrier.add(1);
      workerTasks.back().start(&barrier, asyncFrame);

      RequestContext::setContext(context);

      lock = co_await mutex.co_scoped_lock();
    }
  } catch (...) {
    // Only a fatal error if we failed to create any worker tasks.
    if (workerTasks.empty()) {
      // No need to synchronise here. There are no concurrent tasks running.
      iterationException = exception_wrapper{current_exception()};
    }
  }

  co_await detail::UnsafeResumeInlineSemiAwaitable{barrier.arriveAndWait()};

  if (auto& ex = iterationException ? iterationException : firstException) {
    co_yield co_error(std::move(ex));
  }

  std::vector<detail::collect_all_range_component_t<
      detail::range_reference_t<InputRange>>>
      results;
  results.reserve(tryResults.size());

  for (auto&& tryResult : tryResults) {
    assert(tryResult.hasValue());
    results.emplace_back(std::move(tryResult).value());
  }

  co_return results;
}

template <typename InputRange>
auto collectAllTryWindowed(InputRange awaitables, std::size_t maxConcurrency)
    -> folly::coro::Task<std::vector<detail::collect_all_try_range_component_t<
        detail::range_reference_t<InputRange>>>> {
  assert(maxConcurrency > 0);

  std::vector<detail::collect_all_try_range_component_t<
      detail::range_reference_t<InputRange>>>
      results;

  exception_wrapper iterationException;

  folly::coro::Mutex mutex;

  const Executor::KeepAlive<> executor = co_await co_current_executor;
  const CancellationToken& cancelToken = co_await co_current_cancellation_token;

  auto iter = access::begin(awaitables);
  const auto iterEnd = access::end(awaitables);

  using iterator_t = decltype(iter);
  using awaitable_t = typename std::iterator_traits<iterator_t>::value_type;
  using result_t = semi_await_result_t<awaitable_t>;

  auto makeWorker = [&]() -> detail::BarrierTask {
    auto lock =
        co_await co_viaIfAsync(executor.get_alias(), mutex.co_scoped_lock());

    while (!iterationException && iter != iterEnd) {
      const std::size_t thisIndex = results.size();
      std::optional<awaitable_t> awaitable;

      try {
        results.emplace_back();
        awaitable.emplace(*iter);
        ++iter;
      } catch (...) {
        iterationException = exception_wrapper{current_exception()};
      }

      if (!awaitable) {
        co_return;
      }

      lock.unlock();

      detail::collect_all_try_range_component_t<
          detail::range_reference_t<InputRange>>
          result;

      try {
        if constexpr (detail::collect_detail::can_co_awaitTry_v<awaitable_t>) {
          result = co_await co_awaitTry(co_viaIfAsync(
              executor.get_alias(),
              co_withCancellation(cancelToken, std::move(*awaitable))));
        } else if constexpr (std::is_void_v<result_t>) {
          co_await co_viaIfAsync(
              executor.get_alias(),
              co_withCancellation(cancelToken, std::move(*awaitable)));
          result.emplace();
        } else {
          result.emplace(
              co_await co_viaIfAsync(
                  executor.get_alias(),
                  co_withCancellation(cancelToken, std::move(*awaitable))));
        }
      } catch (...) {
        result.emplaceException(current_exception());
      }

      lock =
          co_await co_viaIfAsync(executor.get_alias(), mutex.co_scoped_lock());

      try {
        results[thisIndex] = std::move(result);
      } catch (...) {
        results[thisIndex].emplaceException(current_exception());
      }
    }
  };

  std::vector<detail::BarrierTask> workerTasks;

  detail::Barrier barrier{1};

  // Save the initial context and restore it after starting each task
  // as the task may have modified the context before suspending and we
  // want to make sure the next task is started with the same initial
  // context.
  const auto context = RequestContext::saveContext();

  auto& asyncFrame = co_await detail::co_current_async_stack_frame;

  try {
    auto lock = co_await mutex.co_scoped_lock();
    while (!iterationException && iter != iterEnd &&
           workerTasks.size() < maxConcurrency) {
      // Unlock the mutex before starting the child operation so that
      // it can consume as many results synchronously as it can before
      // returning here and letting us potentially spawn another task.
      // This can avoid spawning more worker coroutines than is necessary
      // to consume all of the awaitables.
      lock.unlock();

      workerTasks.push_back(makeWorker());
      barrier.add(1);
      workerTasks.back().start(&barrier, asyncFrame);

      RequestContext::setContext(context);

      lock = co_await mutex.co_scoped_lock();
    }
  } catch (...) {
    // Failure to create a worker is an error if we failed
    // to create _any_ workers. As long as we created one then
    // the algorithm should still be able to make forward progress.
    if (workerTasks.empty()) {
      iterationException = exception_wrapper{current_exception()};
    }
  }

  co_await detail::UnsafeResumeInlineSemiAwaitable{barrier.arriveAndWait()};

  if (iterationException) {
    co_yield co_error(std::move(iterationException));
  }

  co_return results;
}

template <typename InputRange>
auto makeUnorderedAsyncGenerator(AsyncScope& scope, InputRange awaitables)
    -> AsyncGenerator<detail::async_generator_from_awaitable_range_item_t<
        InputRange,
        false>&&> {
  return detail::makeUnorderedAsyncGeneratorImpl(
      scope, std::move(awaitables), std::bool_constant<false>{});
}

template <typename InputRange>
auto makeUnorderedTryAsyncGenerator(AsyncScope& scope, InputRange awaitables)
    -> AsyncGenerator<detail::async_generator_from_awaitable_range_item_t<
        InputRange,
        true>&&> {
  return detail::makeUnorderedAsyncGeneratorImpl(
      scope, std::move(awaitables), std::bool_constant<true>{});
}

template <typename InputRange>
auto makeUnorderedAsyncGenerator(
    CancellableAsyncScope& scope, InputRange awaitables)
    -> AsyncGenerator<detail::async_generator_from_awaitable_range_item_t<
        InputRange,
        false>&&> {
  return detail::makeUnorderedAsyncGeneratorImpl(
      scope, std::move(awaitables), std::bool_constant<false>{});
}

template <typename InputRange>
auto makeUnorderedTryAsyncGenerator(
    CancellableAsyncScope& scope, InputRange awaitables)
    -> AsyncGenerator<detail::async_generator_from_awaitable_range_item_t<
        InputRange,
        true>&&> {
  return detail::makeUnorderedAsyncGeneratorImpl(
      scope, std::move(awaitables), std::bool_constant<true>{});
}

template <typename SemiAwaitable, typename... SemiAwaitables>
auto collectAny(SemiAwaitable&& awaitable, SemiAwaitables&&... awaitables)
    -> folly::coro::Task<std::pair<
        std::size_t,
        folly::Try<detail::collect_any_component_t<
            SemiAwaitable,
            SemiAwaitables...>>>> {
  return detail::collectAnyImpl(
      std::make_index_sequence<sizeof...(SemiAwaitables) + 1>{},
      static_cast<SemiAwaitable&&>(awaitable),
      static_cast<SemiAwaitables&&>(awaitables)...);
}

template <typename... SemiAwaitables>
auto collectAnyWithoutException(SemiAwaitables&&... awaitables)
    -> folly::coro::Task<std::pair<
        std::size_t,
        folly::Try<detail::collect_any_component_t<SemiAwaitables...>>>> {
  return detail::collectAnyWithoutExceptionImpl(
      std::make_index_sequence<sizeof...(SemiAwaitables)>{},
      static_cast<SemiAwaitables&&>(awaitables)...);
}

template <typename... SemiAwaitables>
auto collectAnyNoDiscard(SemiAwaitables&&... awaitables)
    -> folly::coro::Task<std::tuple<detail::collect_all_try_component_t<
        std::remove_cvref_t<SemiAwaitables>>...>> {
  return detail::collectAnyNoDiscardImpl(
      std::make_index_sequence<sizeof...(SemiAwaitables)>{},
      static_cast<SemiAwaitables&&>(awaitables)...);
}

template <typename InputRange>
auto collectAnyRange(InputRange awaitables) -> folly::coro::Task<std::pair<
    size_t,
    folly::Try<detail::collect_all_range_component_t<
        detail::range_reference_t<InputRange>>>>> {
  const CancellationToken& parentCancelToken =
      co_await co_current_cancellation_token;
  const CancellationSource cancelSource;
  const CancellationToken cancelToken =
      cancellation_token_merge(parentCancelToken, cancelSource.getToken());

  std::pair<
      size_t,
      folly::Try<detail::collect_all_range_component_t<
          detail::range_reference_t<InputRange>>>>
      firstCompletion;
  firstCompletion.first = size_t(-1);

  using awaitable_type =
      std::remove_cvref_t<detail::range_reference_t<InputRange>>;
  auto makeTask = [&](awaitable_type semiAwaitable, size_t index)
      -> folly::coro::Task<void> {
    auto result = co_await folly::coro::co_awaitTry(std::move(semiAwaitable));
    if (!cancelSource.requestCancellation()) {
      // This is first entity to request cancellation.
      firstCompletion.first = index;
      firstCompletion.second = std::move(result);
    }
  };

  auto tasks = detail::collectMakeInnerTaskVec(awaitables, makeTask);

  co_await folly::coro::co_withCancellation(
      cancelToken, folly::coro::collectAllRange(detail::MoveRange(tasks)));

  co_return firstCompletion;
}

template <typename InputRange>
auto collectAnyWithoutExceptionRange(InputRange awaitables)
    -> folly::coro::Task<std::pair<
        size_t,
        folly::Try<detail::collect_all_range_component_t<
            detail::range_reference_t<InputRange>>>>> {
  const CancellationToken& parentCancelToken =
      co_await co_current_cancellation_token;
  const CancellationSource cancelSource;
  const CancellationToken cancelToken =
      cancellation_token_merge(parentCancelToken, cancelSource.getToken());

  size_t nAwaitables;
  std::atomic<std::size_t> nAwaited = 1;
  std::pair<
      size_t,
      folly::Try<detail::collect_all_range_component_t<
          detail::range_reference_t<InputRange>>>>
      firstValueOrLastException;
  firstValueOrLastException.first = std::numeric_limits<size_t>::max();

  using awaitable_type =
      std::remove_cvref_t<detail::range_reference_t<InputRange>>;
  auto makeTask = [&](awaitable_type semiAwaitable, size_t index)
      -> folly::coro::Task<void> {
    auto result = co_await folly::coro::co_awaitTry(std::move(semiAwaitable));
    if ((result.hasValue() ||
         nAwaited.fetch_add(1, std::memory_order_relaxed) == nAwaitables) &&
        !cancelSource.requestCancellation()) {
      firstValueOrLastException.first = index;
      firstValueOrLastException.second = std::move(result);
    }
  };

  auto tasks = detail::collectMakeInnerTaskVec(awaitables, makeTask);
  nAwaitables = tasks.size();
  co_await folly::coro::co_withCancellation(
      cancelToken, folly::coro::collectAllRange(detail::MoveRange(tasks)));

  co_return firstValueOrLastException;
}

template <typename InputRange>
auto collectAnyNoDiscardRange(InputRange awaitables)
    -> folly::coro::Task<std::vector<detail::collect_all_try_range_component_t<
        detail::range_reference_t<InputRange>>>> {
  const CancellationToken& parentCancelToken =
      co_await co_current_cancellation_token;
  const CancellationSource cancelSource;
  const CancellationToken cancelToken =
      cancellation_token_merge(parentCancelToken, cancelSource.getToken());

  std::vector<detail::collect_all_try_range_component_t<
      detail::range_reference_t<InputRange>>>
      results;

  using awaitable_type =
      std::remove_cvref_t<detail::range_reference_t<InputRange>>;
  auto makeTask = [&](awaitable_type semiAwaitable, size_t index)
      -> folly::coro::Task<void> {
    auto result = co_await folly::coro::co_awaitTry(std::move(semiAwaitable));
    cancelSource.requestCancellation();
    results[index] = std::move(result);
  };

  auto tasks = detail::collectMakeInnerTaskVec(awaitables, makeTask);

  results.resize(tasks.size());
  co_await folly::coro::co_withCancellation(
      cancelToken, folly::coro::collectAllRange(detail::MoveRange(tasks)));

  co_return results;
}

} // namespace coro
} // namespace folly

#endif // FOLLY_HAS_COROUTINES
