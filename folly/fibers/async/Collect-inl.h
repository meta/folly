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

namespace folly {
namespace fibers {
namespace async {

namespace detail {
/**
 * Wrapper around the input iterators to return the wrapped functors
 */
template <class InnerIterator>
struct await_iterator {
  /**
   * Wrapper around the input functor to apply `init_await` on invocation
   */
  struct AwaitWrapper {
    explicit AwaitWrapper(InnerIterator it) : it_(std::move(it)) {}
    auto operator()() { return init_await((*it_)()); }

   private:
    InnerIterator it_;
  };

  using iterator_category = std::input_iterator_tag;
  using value_type = AwaitWrapper;
  using difference_type = std::size_t;
  using pointer = value_type*;
  using reference = value_type&;

  explicit await_iterator(InnerIterator it) : it_(it) {}

  await_iterator& operator++() {
    ++it_;
    return *this;
  }

  await_iterator operator++(int) {
    await_iterator retval = *this;
    ++(*this);
    return retval;
  }

  bool operator==(await_iterator other) const { return it_ == other.it_; }

  value_type operator*() const { return AwaitWrapper(it_); }

 private:
  InnerIterator it_;
};

template <typename Func, typename... Ts, std::size_t... I>
void foreach(std::index_sequence<I...>, Func&& func, Ts&&... ts) {
  (func(index_constant<I>{}, std::forward<Ts>(ts)), ...);
}

template <typename Out, typename T>
typename std::enable_if<
    !std::is_same<std::invoke_result_t<T>, Async<void>>::value,
    Async<void>>::type
executeAndMaybeAssign(Out& outref, T&& task) {
  tryEmplaceWith(outref, [task = static_cast<T&&>(task)] {
    return init_await(task());
  });
  return {};
}

template <typename Out, typename T>
typename std::enable_if<
    std::is_same<std::invoke_result_t<T>, Async<void>>::value,
    Async<void>>::type
executeAndMaybeAssign(Out& outref, T&& task) {
  tryEmplaceWith(outref, [task = static_cast<T&&>(task)] {
    init_await(task());
    return unit;
  });
  return {};
}

template <typename T>
using collected_result_t =
    lift_unit_t<async_inner_type_t<std::invoke_result_t<T>>>;

template <
    typename T,
    typename... Ts,
    typename TResult =
        std::tuple<collected_result_t<T>, collected_result_t<Ts>...>>
Async<TResult> collectAllImpl(T&& firstTask, Ts&&... tasks) {
  std::tuple<Try<collected_result_t<T>>, Try<collected_result_t<Ts>>...> result;
  size_t numPending{std::tuple_size<TResult>::value};
  Baton b;

  auto taskFunc = [&](auto& outref, auto&& task) -> Async<void> {
    await_async(
        executeAndMaybeAssign(outref, std::forward<decltype(task)>(task)));

    --numPending;
    if (numPending == 0) {
      b.post();
    }

    return {};
  };

  auto& fm = FiberManager::getFiberManager();
  detail::foreach(
      std::index_sequence_for<Ts...>{},
      [&](auto i, auto&& task) {
        addFiber(
            [&]() {
              return taskFunc(
                  std::get<i + 1>(result), std::forward<decltype(task)>(task));
            },
            fm);
      },
      std::forward<Ts>(tasks)...);

  // Use the current fiber to execute first task
  await_async(taskFunc(std::get<0>(result), std::forward<T>(firstTask)));

  // Wait for other tasks to complete
  b.wait();

  return unwrapTryTuple(result);
}
} // namespace detail

template <class InputIterator, typename FuncType, typename ResultType>
Async<std::vector<
    typename std::enable_if<
        !std::is_same<ResultType, Async<void>>::value,
        async_inner_type_t<ResultType>>::
        type>> inline collectAll(InputIterator first, InputIterator last) {
  return Async(
      folly::fibers::collectAll(
          detail::await_iterator<InputIterator>(first),
          detail::await_iterator<InputIterator>(last)));
}

template <class InputIterator, typename FuncType, typename ResultType>
typename std::
    enable_if<std::is_same<ResultType, Async<void>>::value, Async<void>>::
        type inline collectAll(InputIterator first, InputIterator last) {
  folly::fibers::collectAll(
      detail::await_iterator<InputIterator>(first),
      detail::await_iterator<InputIterator>(last));

  return {};
}

template <typename... Ts, typename TResult>
Async<TResult> collectAll(Ts&&... tasks) {
  static_assert(sizeof...(Ts) > 0);
  return detail::collectAllImpl(std::forward<Ts>(tasks)...);
}

} // namespace async
} // namespace fibers
} // namespace folly
