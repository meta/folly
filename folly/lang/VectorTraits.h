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

#include <type_traits>
#include <vector>

// Implements is_vector_bool_reference from wg21.link/p3719
namespace folly {
#if defined(__GLIBCXX__)
template <typename T>
constexpr bool is_vector_bool_reference_v =
    std::is_same_v<T, std::_Bit_reference>;

#elif defined(_LIBCPP_VERSION)
template <typename T>
constexpr bool is_vector_bool_reference_v = false;

template <typename Alloc>
constexpr bool
    is_vector_bool_reference_v<std::__bit_reference<std::vector<bool, Alloc>>> =
        true;

#elif defined(_MSVC_STL_VERSION)
template <typename T>
constexpr bool is_vector_bool_reference_v = false;

template <typename T>
constexpr bool is_vector_bool_reference_v<std::_Vb_reference<T>> = true;
#endif

template <typename T>
concept vector_bool_reference = is_vector_bool_reference_v<T>;

} // namespace folly
