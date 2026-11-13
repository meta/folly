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

#include <folly/Portability.h>
#include <folly/Preprocessor.h>

#include <type_traits>

namespace folly {

/**
 * @function for_each
 *
 * folly::for_each is a generalized iteration algorithm. Example:
 *
 *  auto one = std::make_tuple(1, 2, 3);
 *  auto two = std::vector<int>{1, 2, 3};
 *  auto func = [](auto element, auto index) {
 *    cout << index << " : " << element << endl;
 *  };
 *  folly::for_each(one, func);
 *  folly::for_each(two, func);
 *
 * The for_each function allows iteration through sequences, these can either be
 * runtime sequences (i.e. entities for which std::begin and std::end work) or
 * compile time sequences (as deemed by the presence of std::tuple_length<> and
 * member get<> or ADL get<> functions).
 *
 * If a sequence type is both a runtime sequence (aka range) and a compile-time
 * sequence (aka tuple), then it is treated as a range in preference to a tuple.
 * An example of such a type is std::array.
 *
 * The function is made to provide a convenient library based alternative to the
 * proposal p0589r0, which aims to generalize the range based for loop even
 * further to work with compile time sequences.
 *
 * A drawback of using range based for loops is that sometimes you do not have
 * access to the index within the range. This provides easy access to that, even
 * with compile time sequences.
 *
 * And breaking out is easy:
 *
 *  auto range_one = std::vector<int>{1, 2, 3};
 *  auto range_two = std::make_tuple(1, 2, 3);
 *  auto func = [](auto ele, auto index) {
 *    cout << "Element at index " << index << " : " << ele;
 *    if (index == 1) {
 *      return folly::loop_break;
 *    }
 *    return folly::loop_continue;
 *  };
 *  folly::for_each(range_one, func);
 *  folly::for_each(range_two, func);
 *
 * A simple use case would be when using futures, if the user was doing calls to
 * n servers then they would accept the callback with the futures like this:
 *
 *  auto vec = std::vector<std::future<int>>{request_one(), ...};
 *  when_all(vec.begin(), vec.end()).then([](auto futures) {
 *    folly::for_each(futures, [](auto& fut) { ... });
 *  });
 *
 * Now when this code switches to use tuples instead of the runtime std::vector,
 * then the loop does not need to change, the code will still work just fine:
 *
 *  when_all(future_one, future_two, future_three).then([](auto futures) {
 *    folly::for_each(futures, [](auto& fut) { ... });
 *  });
 */
template <typename Range, typename Func>
constexpr Func for_each(Range&& range, Func func);

/**
 * The user should return loop_break and loop_continue if they want to iterate
 * in such a way that they can preemptively stop the loop and break out when
 * certain conditions are met.
 */
namespace for_each_detail {
enum class LoopControl : bool { BREAK, CONTINUE };
} // namespace for_each_detail

constexpr auto loop_break = for_each_detail::LoopControl::BREAK;
constexpr auto loop_continue = for_each_detail::LoopControl::CONTINUE;

/**
 * Utility method to help access elements of a sequence with one uniform
 * interface.
 *
 * This can be useful for example when you are looping through a sequence and
 * want to modify another sequence based on the information in the current
 * sequence:
 *
 *  auto range_one = std::make_tuple(1, 2, 3);
 *  auto range_two = std::make_tuple(4, 5, 6);
 *  folly::for_each(range_one, [&range_two](auto ele, auto index) {
 *    folly::fetch(range_two, index) = ele;
 *  });
 *
 * For ranges, this works by first trying to use the iterator class if the
 * iterator has been marked to be a random access iterator. This should be
 * inspectable via the std::iterator_traits traits class. If the iterator class
 * is not present or is not a random access iterator then the implementation
 * falls back to trying to use the indexing operator (operator[]) to fetch the
 * required element.
 */
template <typename Sequence, typename Index>
constexpr decltype(auto) fetch(Sequence&& sequence, Index&& index);

} // namespace folly

/**
 * Everything below is deprecated.
 */

#include <folly/container/Foreach-inl.h>
