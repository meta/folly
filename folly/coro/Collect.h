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

#pragma once

#include <folly/Try.h>
#include <folly/Unit.h>
#include <folly/container/Access.h>
#include <folly/container/Iterator.h>
#include <folly/coro/AsyncGenerator.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/Coroutine.h>
#include <folly/coro/Task.h>
#include <folly/coro/ViaIfAsync.h>
#include <folly/coro/detail/PickTaskWrapper.h>
#include <folly/coro/detail/Traits.h>
// `collectAll(coroFutureInt())` makes a `safe_task`
#include <folly/coro/safe/SafeTask.h>
// `collectAll(memberTask())` makes a `now_task`
#include <folly/coro/safe/NowTask.h>

#include <iterator>
#include <tuple>
#include <type_traits>

#if FOLLY_HAS_COROUTINES

namespace folly {
namespace coro {
namespace detail {

template <typename SemiAwaitable>
using collect_all_try_component_t = folly::Try<decay_rvalue_reference_t<
    lift_lvalue_reference_t<semi_await_result_t<SemiAwaitable>>>>;

template <typename SemiAwaitable>
using collect_all_component_t =
    decay_rvalue_reference_t<lift_unit_t<semi_await_result_t<SemiAwaitable>>>;

template <typename SemiAwaitable>
using collect_all_range_component_t = decay_rvalue_reference_t<
    lift_lvalue_reference_t<lift_unit_t<semi_await_result_t<SemiAwaitable>>>>;

template <typename SemiAwaitable>
using collect_all_try_range_component_t =
    collect_all_try_component_t<SemiAwaitable>;

template <typename... SemiAwaitables>
using collect_any_component_t = std::common_type_t<
    decay_rvalue_reference_t<semi_await_result_t<SemiAwaitables>>...>;

template <typename Range>
using range_iterator_t = decltype(access::begin(std::declval<Range&>()));

template <typename Iterator>
using iterator_reference_t = typename std::iterator_traits<Iterator>::reference;

template <typename Range>
using range_reference_t = iterator_reference_t<range_iterator_t<Range>>;

// A bare-bones std::range implementation that is similar to ranges::views::move
template <typename Container>
class MoveRange {
 public:
  explicit MoveRange(Container& container) : container_(container) {}

  auto begin() { return std::make_move_iterator(container_.begin()); }
  auto end() { return std::make_move_iterator(container_.end()); }

 private:
  Container& container_;
};

// Future: Apply `value_or_fatal` to the task if the entire collection process
// is value-only awaitable.  This would require reworking the implementation a
// bit, since e.g. `cancellation_token_merge` can throw `bad_alloc`.
template <typename... SemiAwaitables>
using CollectAllTask = pick_task_wrapper<
    std::tuple<collect_all_component_t<std::remove_cvref_t<SemiAwaitables>>...>,
    std::min(
        {safe_alias::maybe_value, lenient_safe_alias_of_v<SemiAwaitables>...}),
    (folly::ext::must_use_immediately_v<SemiAwaitables> || ...)>;

template <typename... SemiAwaitables>
using CollectAllTryTask = pick_task_wrapper<
    std::tuple<
        collect_all_try_component_t<std::remove_cvref_t<SemiAwaitables>>...>,
    std::min(
        {safe_alias::maybe_value, lenient_safe_alias_of_v<SemiAwaitables>...}),
    (folly::ext::must_use_immediately_v<SemiAwaitables> || ...)>;

} // namespace detail

///////////////////////////////////////////////////////////////////////////
// collectAll(SemiAwaitable<Ts>...) -> SemiAwaitable<std::tuple<Ts...>>
//
// The collectAll() function can be used to concurrently co_await on multiple
// SemiAwaitable objects and continue once they are all complete.
//
// collectAll() accepts an arbitrary number of SemiAwaitable objects and returns
// a SemiAwaitable object that will complete with a std::tuple of the results.
//
// When the returned SemiAwaitable object is co_awaited it will launch
// a new coroutine for awaiting each input awaitable in-turn.
//
// Note that coroutines for awaiting the input awaitables of later arguments
// will not be launched until the prior coroutine reaches its first suspend
// point. This means that awaiting multiple sub-tasks that all complete
// synchronously will still execute them sequentially on the current thread.
//
// If any of the input operations complete with an exception then it will
// request cancellation of any outstanding tasks and the whole collectAll()
// operation will complete with an exception once all of the operations
// have completed.  Any partial results will be discarded. If multiple
// operations fail with an exception then the exception from the first task
// to fail will be rethrown and subsequent errors are discarded.
//
// If you need to know which operation failed or you want to handle partial
// failures then you can use the folly::coro::collectAllTry() instead which
// returns a tuple of Try<T> objects instead of a tuple of values.
//
// Example: Serially awaiting multiple operations (slower)
//   folly::coro::Task<Foo> doSomething();
//   folly::coro::Task<Bar> doSomethingElse();
//
//   Foo result1 = co_await doSomething();
//   Bar result2 = co_await doSomethingElse();
//
// Example: Concurrently awaiting multiple operations (faster) C++17-only.
//   auto [result1, result2] =
//       co_await folly::coro::collectAll(doSomething(), doSomethingElse());
//
template <typename... SemiAwaitables>
// Do NOT take awaitables by-reference, that would break `now_task` safety.
auto collectAll(SemiAwaitables... awaitables)
    -> detail::CollectAllTask<SemiAwaitables...>;

///////////////////////////////////////////////////////////////////////////
// collectAllTry(SemiAwaitable<Ts>...)
//    -> SemiAwaitable<std::tuple<Try<Ts>...>>
//
// Like the collectAll() function, the collectAllTry() function can be used to
// concurrently await multiple input SemiAwaitable objects.
//
// The collectAllTry() function differs from collectAll() in that it produces a
// tuple of Try<T> objects rather than a tuple of the values.
// This allows the caller to inspect the success/failure of individual
// operations and handle partial failures but has a less-convenient interface
// than collectAll().
//
// It also differs in that failure of one subtask does _not_ request
// cancellation of the other subtasks.
//
// Example: Handling partial failure with collectAllTry()
//    folly::coro::Task<Foo> doSomething();
//    folly::coro::Task<Bar> doSomethingElse();
//
//    auto [result1, result2] = co_await folly::coro::collectAllTry(
//        doSomething(), doSomethingElse());
//
//    if (result1.hasValue()) {
//      Foo& foo = result1.value();
//      process(foo);
//    } else {
//      logError("doSomething() failed", result1.exception());
//    }
//
//    if (result2.hasValue()) {
//      Bar& bar = result2.value();
//      process(bar);
//    } else {
//      logError("doSomethingElse() failed", result2.exception());
//    }
//
template <typename... SemiAwaitables>
auto collectAllTry(SemiAwaitables... awaitables)
    -> detail::CollectAllTryTask<SemiAwaitables...>;

////////////////////////////////////////////////////////////////////////
// collectAllRange(RangeOf<SemiAwaitable<T>>&&)
//   -> SemiAwaitable<std::vector<T>>
//
// The collectAllRange() function can be used to concurrently await a collection
// of SemiAwaitable objects, returning a std::vector of the individual results
// in the same order as the input once all operations have completed.
//
// If any of the operations fail with an exception then requests cancellation of
// any outstanding operations and the entire operation fails with an exception,
// discarding any partial results. If more than one operation fails with an
// exception then the exception from task that failed first (in time) is
// rethrown. Other results and exceptions are discarded.
//
// If you need to be able to distinguish which operation failed or handle
// partial failures then use collectAllTryRange() instead.
//
// Note that the expression `*it` must be SemiAwaitable.
// This typically means that containers of Task<T> must be adapted to produce
// moved-elements by applying the ranges::views::move transform.
// e.g.
//
//   std::vector<Task<T>> tasks = ...;
//   std::vector<T> vals = co_await collectAllRange(tasks |
//   ranges::views::move);
//
template <typename InputRange>
  requires(!std::is_void_v<
           semi_await_result_t<detail::range_reference_t<InputRange>>>)
auto collectAllRange(InputRange awaitables)
    -> folly::coro::Task<std::vector<detail::collect_all_range_component_t<
        detail::range_reference_t<InputRange>>>>;
template <typename InputRange>
  requires std::is_void_v<
      semi_await_result_t<detail::range_reference_t<InputRange>>>
auto collectAllRange(InputRange awaitables) -> folly::coro::Task<void>;

////////////////////////////////////////////////////////////////////////////
// collectAllTryRange(RangeOf<SemiAwaitable<T>>&&)
//    -> SemiAwaitable<std::vector<folly::Try<T>>>
//
// The collectAllTryRange() function can be used to concurrently await a
// collection of SemiAwaitable objects and produces a std::vector of
// Try<T> objects in the same order as the input once all of the input
// operations have completed.
//
// The success/failure of individual results can be inspected by calling
// .hasValue() or .hasException() on the elements of the returned vector.
template <typename InputRange>
auto collectAllTryRange(InputRange awaitables)
    -> folly::coro::Task<std::vector<detail::collect_all_try_range_component_t<
        detail::range_reference_t<InputRange>>>>;

// collectAllRange()/collectAllTryRange() overloads that simplifies the
// common-case where an rvalue std::vector<SemiAwaitable> is passed.
//
// This avoids the caller needing to pipe the input through ranges::views::move
// transform to force the elements to be rvalue-references since the
// std::vector<T>::reference type is T& rather than T&& and some awaitables,
// such as Task<U>, are not lvalue awaitable.
template <typename SemiAwaitable>
auto collectAllRange(std::vector<SemiAwaitable> awaitables)
    -> decltype(collectAllRange(detail::MoveRange(awaitables))) {
  co_return co_await collectAllRange(detail::MoveRange(awaitables));
}

template <typename SemiAwaitable>
auto collectAllTryRange(std::vector<SemiAwaitable> awaitables)
    -> decltype(collectAllTryRange(detail::MoveRange(awaitables))) {
  co_return co_await collectAllTryRange(detail::MoveRange(awaitables));
}

namespace detail {
template <typename InputRange, bool IsTry>
using async_generator_from_awaitable_range_item_t = conditional_t<
    IsTry,
    collect_all_try_range_component_t<range_reference_t<InputRange>>,
    collect_all_range_component_t<range_reference_t<InputRange>>>;
}

////////////////////////////////////////////////////////////////////////////
// makeUnorderedAsyncGenerator(AsyncScope&,
// RangeOf<SemiAwaitable<T>>&&) -> AsyncGenerator<T&&>
// makeUnorderedTryAsyncGenerator(AsyncScope&,
// RangeOf<SemiAwaitable<T>>&&) -> AsyncGenerator<Try<T>&&>

// Returns an AsyncGenerator that yields results of passed-in awaitables in
// order of completion.
// Destroying or cancelling the AsyncGenerator cancels the remaining awaitables.
//
// makeUnorderedAsyncGenerator cancels all remaining
// awaitables when any of them fail with an exception. Any results obtained
// before the failure are still returned via the generator, then the first
// exception in time. makeUnorderedTryAsyncGenerator does not
// cancel awaitables when one fails, and yields all results even when cancelled.
//
// Awaitables are attached to the passed-in AsyncScope.

template <typename InputRange>
auto makeUnorderedAsyncGenerator(AsyncScope& scope, InputRange awaitables)
    -> AsyncGenerator<detail::async_generator_from_awaitable_range_item_t<
        InputRange,
        false>&&>;
template <typename InputRange>
auto makeUnorderedTryAsyncGenerator(AsyncScope& scope, InputRange awaitables)
    -> AsyncGenerator<detail::async_generator_from_awaitable_range_item_t<
        InputRange,
        true>&&>;

template <typename SemiAwaitable>
auto makeUnorderedAsyncGenerator(
    AsyncScope& scope, std::vector<SemiAwaitable> awaitables)
    -> decltype(makeUnorderedAsyncGenerator(
        scope, detail::MoveRange(awaitables))) {
  auto gen = makeUnorderedAsyncGenerator(scope, detail::MoveRange(awaitables));
  while (true) {
    co_yield co_result(co_await co_awaitTry(gen.next()));
  }
}
template <typename SemiAwaitable>
auto makeUnorderedTryAsyncGenerator(
    AsyncScope& scope, std::vector<SemiAwaitable> awaitables)
    -> decltype(makeUnorderedTryAsyncGenerator(
        scope, detail::MoveRange(awaitables))) {
  auto gen =
      makeUnorderedTryAsyncGenerator(scope, detail::MoveRange(awaitables));
  while (true) {
    co_yield co_result(co_await co_awaitTry(gen.next()));
  }
}

// Can also be used with CancellableAsyncScope

template <typename InputRange>
auto makeUnorderedAsyncGenerator(
    CancellableAsyncScope& scope, InputRange awaitables)
    -> AsyncGenerator<detail::async_generator_from_awaitable_range_item_t<
        InputRange,
        false>&&>;
template <typename InputRange>
auto makeUnorderedTryAsyncGenerator(
    CancellableAsyncScope& scope, InputRange awaitables)
    -> AsyncGenerator<detail::async_generator_from_awaitable_range_item_t<
        InputRange,
        true>&&>;

template <typename SemiAwaitable>
auto makeUnorderedAsyncGenerator(
    CancellableAsyncScope& scope, std::vector<SemiAwaitable> awaitables)
    -> decltype(makeUnorderedAsyncGenerator(
        scope, detail::MoveRange(awaitables))) {
  auto gen = makeUnorderedAsyncGenerator(scope, detail::MoveRange(awaitables));
  while (true) {
    co_yield co_result(co_await co_awaitTry(gen.next()));
  }
}
template <typename SemiAwaitable>
auto makeUnorderedTryAsyncGenerator(
    CancellableAsyncScope& scope, std::vector<SemiAwaitable> awaitables)
    -> decltype(makeUnorderedTryAsyncGenerator(
        scope, detail::MoveRange(awaitables))) {
  auto gen =
      makeUnorderedTryAsyncGenerator(scope, detail::MoveRange(awaitables));
  while (true) {
    co_yield co_result(co_await co_awaitTry(gen.next()));
  }
}

///////////////////////////////////////////////////////////////////////////////
// collectAllWindowed(RangeOf<SemiAwaitable<T>>&&, size_t maxConcurrency)
//   -> SemiAwaitable<std::vector<T>>
//
// collectAllWindowed(RangeOf<SemiAwaitable<void>>&&, size_t maxConcurrency)
//   -> SemiAwaitable<void>
//
// Await each of the input awaitables in the range, allowing at most
// 'maxConcurrency' of these input awaitables to be concurrently awaited
// at any one point in time.
//
// If any of the input awaitables fail with an exception then requests
// cancellation of any incomplete operations and fails the whole
// operation with an exception. If multiple input awaitables fail with
// an exception then the exception from the first task to fail (in time)
// will be rethrown and the rest of the results will be discarded.
//
// If there is an exception thrown while iterating over the input-range then
// it will still guarantee that any prior awaitables in the input-range will
// run to completion before completing the collectAllWindowed() operation with
// the exception thrown during iteration.
//
// The resulting std::vector will contain the results in the corresponding
// order of their respective awaitables in the input range.
template <typename InputRange>
  requires std::is_void_v<
      semi_await_result_t<detail::range_reference_t<InputRange>>>
auto collectAllWindowed(InputRange awaitables, std::size_t maxConcurrency)
    -> folly::coro::Task<void>;
template <typename InputRange>
  requires(!std::is_void_v<
           semi_await_result_t<detail::range_reference_t<InputRange>>>)
auto collectAllWindowed(InputRange awaitables, std::size_t maxConcurrency)
    -> folly::coro::Task<std::vector<detail::collect_all_range_component_t<
        detail::range_reference_t<InputRange>>>>;

///////////////////////////////////////////////////////////////////////////////
// collectAllTryWindowed(RangeOf<SemiAwaitable<T>>&, size_t maxConcurrency)
//   -> SemiAwaitable<std::vector<folly::Try<T>>>
//
// Concurrently awaits a collection of awaitable with bounded concurrency,
// producing a vector of Try values containing each of the results.
//
// The resulting std::vector will contain the results in the corresponding
// order of their respective awaitables in the input range.
//
// Note that the whole operation may still complete with an exception if
// iterating over the awaitables fails with an exception (eg. if you pass
// a Generator<Task<T>&&> and the generator throws an exception).
template <typename InputRange>
auto collectAllTryWindowed(InputRange awaitables, std::size_t maxConcurrency)
    -> folly::coro::Task<std::vector<detail::collect_all_try_range_component_t<
        detail::range_reference_t<InputRange>>>>;

// collectAllWindowed()/collectAllTryWindowed() overloads that simplify the
// use of these functions with std::vector<SemiAwaitable>.
template <typename SemiAwaitable>
auto collectAllWindowed(
    std::vector<SemiAwaitable> awaitables, std::size_t maxConcurrency)
    -> decltype(collectAllWindowed(
        detail::MoveRange(awaitables), maxConcurrency)) {
  co_return co_await collectAllWindowed(
      detail::MoveRange(awaitables), maxConcurrency);
}

template <typename SemiAwaitable>
auto collectAllTryWindowed(
    std::vector<SemiAwaitable> awaitables, std::size_t maxConcurrency)
    -> decltype(collectAllTryWindowed(
        detail::MoveRange(awaitables), maxConcurrency)) {
  co_return co_await collectAllTryWindowed(
      detail::MoveRange(awaitables), maxConcurrency);
}

///////////////////////////////////////////////////////////////////////////
// collectAny(SemiAwaitable<Ts>...) -> SemiAwaitable<
//   std::pair<std::size_t, folly::Try<std::common_type<Ts...>>>>
//
// The collectAny() function can be used to concurrently co_await on multiple
// SemiAwaitable objects, get the result and index of the first one completing,
// cancel the remaining ones and continue once they are completed.
//
// collectAny() accepts a positive number of SemiAwaitable objects and
// returns a SemiAwaitable object that will complete with a pair containing the
// result of the first one to complete and its index.
//
// collectAny() is built on top of collectAll(), be aware of the coroutine
// starting behavior described in collectAll() documentation.
//
// The result of the first SemiAwaitable is going to be returned, whether it
// is a value or an exception. Any result of the remaining SemiAwaitables will
// be discarded, independently of whether it's a value or an exception.
//
// Example:
//   folly::coro::Task<Foo> getDataOneWay();
//   folly::coro::Task<Foo> getDataAnotherWay();
//
//   std::pair<std::size_t, Try<Foo>> result = co_await folly::coro::collectAny(
//       getDataOneWay(), getDataAnotherWay());
//
template <typename SemiAwaitable, typename... SemiAwaitables>
auto collectAny(SemiAwaitable&& awaitable, SemiAwaitables&&... awaitables)
    -> folly::coro::Task<std::pair<
        std::size_t,
        folly::Try<detail::collect_any_component_t<
            SemiAwaitable,
            SemiAwaitables...>>>>;

///////////////////////////////////////////////////////////////////////////
// collectAnyWithoutException(SemiAwaitable<Ts>...)
//    -> SemiAwaitable<std::pair<std::size_t, folly::Try<T>>>
//
// The collectAnyWithoutException() function is similar to collectAny() in that
// it co_awaits multiple SemiAwaitables and cancels any outstanding operations
// when complete. Unlike collectAny(), it returns the first success, or the last
// exception if all of the SemiAwaitables fail.
//
// collectAnyWithoutException() is built on top of collectAll(), be aware of the
// coroutine starting behavior described in collectAll() documentation.
//
// The result of the first successful SemiAwaitable, or the exception from
// the last SemiAwaitable is returned if none are successful. Any result of the
// remaining SemiAwaitables will be discarded, independently of whether it's a
// value or an exception.
//
// Example:
//   folly::coro::Task<Foo> getDataOneWay();
//   folly::coro::Task<Foo> getDataAnotherWay();
//
//   std::pair<std::size_t, Try<Foo>> result =
//       co_await folly::coro::collectAnyWithoutException(
//           getDataOneWay(), getDataAnotherWay());
//
template <typename... SemiAwaitables>
auto collectAnyWithoutException(SemiAwaitables&&... awaitables)
    -> folly::coro::Task<std::pair<
        std::size_t,
        folly::Try<detail::collect_any_component_t<SemiAwaitables...>>>>;

///////////////////////////////////////////////////////////////////////////
// collectAnyNoDiscard(SemiAwaitable<Ts>...) ->
//   SemiAwaitable<std::tuple<folly::Try<Ts>...>>
//
// The collectAnyNoDiscard() function is similar to collectAny() in that it
// co_awaits multiple SemiAwaitables and cancels any outstanding operations once
// at least one has finished. Unlike collectAny(), it returns results from *all*
// SemiAwaitables, including folly::OperationCancelled for operations that were
// cancelled.
//
// collectAnyNoDiscard() is built on top of collectAll(), be aware of the
// coroutine starting behavior described in collectAll() documentation.
//
// The returned tuple contains the results of all the SemiAwaitables.
//
// Example:
//   folly::coro::Task<Foo> getDataOneWay();
//   folly::coro::Task<Bar> getDataAnotherWay();
//
//   std::tuple<folly::Try<Foo>, folly::Try<Bar>> result = co_await
//      folly::coro::collectAnyNoDiscard(getDataOneWay(), getDataAnotherWay());
//
template <typename... SemiAwaitables>
auto collectAnyNoDiscard(SemiAwaitables&&... awaitables)
    -> folly::coro::Task<std::tuple<detail::collect_all_try_component_t<
        std::remove_cvref_t<SemiAwaitables>>...>>;

///////////////////////////////////////////////////////////////////////////
// collectAnyRange(RangeOf<SemiAwaitable<T>>&&)
//   -> SemiAwaitable<std::pair<std::size_t, folly::Try<T>>>
//
// The collectAnyRange() function can be used to concurrently co_await on
// multiple SemiAwaitable objects, get the result and index of the first one
// completing, cancel the remaining ones and continue once they are completed.
//
// collectAnyRange() accepts zero or more SemiAwaitable objects and
// returns a SemiAwaitable object that will complete with a pair containing the
// result of the first one to complete and its index.
//
// collectAnyRange() is built on top of collectAllRange(), be aware of the
// coroutine starting behavior described in collectAll() documentation.
//
// The result of the first SemiAwaitable is going to be returned, whether it
// is a value or an exception. Any result of the remaining SemiAwaitables will
// be discarded, independently of whether it's a value or an exception.
//
// e.g.
//
//   std::vector<Task<T>> tasks = ...;
//   std::pair<size_t, Try<T>> result = co_await collectAnyRange(tasks |
//       ranges::views::move);
//
template <typename InputRange>
auto collectAnyRange(InputRange awaitables) -> folly::coro::Task<std::pair<
    size_t,
    folly::Try<detail::collect_all_range_component_t<
        detail::range_reference_t<InputRange>>>>>;

///////////////////////////////////////////////////////////////////////////
// collectAnyWithoutExceptionRange(RangeOf<SemiAwaitable<T>>&&)
//   -> SemiAwaitable<std::pair<std::size_t, folly::Try<T>>>
//
// The collectAnyWithoutExceptionRange() function is similar to
// collectAnyRange() in that it co_awaits multiple SemiAwaitables and cancels
// any outstanding operations when complete. Unlike collectAnyRange(), it
// returns the first success, or the last exception if all of the SemiAwaitables
// fail.
//
// collectAnyWithoutExceptionRange() is built on top of collectAllRange(), be
// aware of the coroutine starting behavior described in collectAll()
// documentation.
//
// The result of the first successful SemiAwaitable, or the the exception from
// the last SemiAwaitable is returned if none are successful. Any result of the
// remaining SemiAwaitables will be discarded, independently of whether it's a
// value or an exception.
//
// Example:
//   std::vector<Task<T>> tasks = ...;
//   std::pair<size_t, Try<T>> result =
//       co_await collectAnyWithoutExceptionRange(tasks | ranges::views::move);
//
template <typename InputRange>
auto collectAnyWithoutExceptionRange(InputRange awaitables)
    -> folly::coro::Task<std::pair<
        size_t,
        folly::Try<detail::collect_all_range_component_t<
            detail::range_reference_t<InputRange>>>>>;

///////////////////////////////////////////////////////////////////////////
// collectAnyNoDiscardRange(RangeOf<SemiAwaitable<T>>&&)
//    -> SemiAwaitable<std::vector<folly::Try<T>>>
//
// The collectAnyNoDiscardRange() function is similar to collectAnyRange() in
// that it co_awaits multiple SemiAwaitables and cancels any outstanding
// operations once at least one has finished. Unlike collectAnyRange(), it
// returns results from *all* SemiAwaitables, including
// folly::OperationCancelled for operations that were cancelled.
//
// collectAnyNoDiscardRange() is built on top of collectAllRange(), be aware of
// the coroutine starting behavior described in collectAll() documentation.
//
// The success/failure of individual results can be inspected by calling
// .hasValue() or .hasException() on the elements of the returned vector.
//
// Example:
//   folly::coro::Task<Foo> getDataOneWay();
//   folly::coro::Task<Foo> getDataAnotherWay();
//
//   std::vector<folly::Try<Foo>> result = co_await
//      folly::coro::collectAnyNoDiscard(getDataOneWay(), getDataAnotherWay());
//
template <typename InputRange>
auto collectAnyNoDiscardRange(InputRange awaitables)
    -> folly::coro::Task<std::vector<detail::collect_all_try_range_component_t<
        detail::range_reference_t<InputRange>>>>;

// collectAnyRange()/collectAnyWithoutExceptionRange()/collectAnyNoDiscardRange()
// overloads that simplifies the common-case where an rvalue
// std::vector<SemiAwaitable> is passed.
//
// This avoids the caller needing to pipe the input through ranges::views::move
// transform to force the elements to be rvalue-references since the
// std::vector<T>::reference type is T& rather than T&& and some awaitables,
// such as Task<U>, are not lvalue awaitable.
template <typename SemiAwaitable>
auto collectAnyRange(std::vector<SemiAwaitable> awaitables)
    -> decltype(collectAnyRange(detail::MoveRange(awaitables))) {
  co_return co_await collectAnyRange(detail::MoveRange(awaitables));
}
template <typename SemiAwaitable>
auto collectAnyWithoutExceptionRange(std::vector<SemiAwaitable> awaitables)
    -> decltype(collectAnyWithoutExceptionRange(
        detail::MoveRange(awaitables))) {
  co_return co_await collectAnyWithoutExceptionRange(
      detail::MoveRange(awaitables));
}
template <typename SemiAwaitable>
auto collectAnyNoDiscardRange(std::vector<SemiAwaitable> awaitables)
    -> decltype(collectAnyNoDiscardRange(detail::MoveRange(awaitables))) {
  co_return co_await collectAnyNoDiscardRange(detail::MoveRange(awaitables));
}

} // namespace coro
} // namespace folly

#endif // FOLLY_HAS_COROUTINES

#include <folly/coro/Collect-inl.h>
