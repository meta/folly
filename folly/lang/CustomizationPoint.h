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

#include <folly/Portability.h>

//  FOLLY_DEFINE_CPO
//
//  Helper for portably defining customization-point objects (CPOs).
//
//  The customization-point object must be placed in a nested namespace to avoid
//  potential conflicts with customizations defined as friend-functions of types
//  defined in the same namespace as the CPO.
#define FOLLY_DEFINE_CPO(Type, Name) \
  namespace folly_cpo__ {            \
  inline constexpr Type Name{};      \
  }                                  \
  using namespace folly_cpo__;

namespace folly {

//  cpo_t<CPO>
//
//  Helper type-trait for obtaining the type of customisation point object.
//
//  This can be useful for defining overloads of tag_invoke() where CPOs
//  are defined in terms of ADL calls to tag_invoke().
//
//  For example:
//   FOLLY_DEFINE_CPO(DoSomething_fn, doSomething)
//
//   class SomeClass {
//     ...
//     friend void tag_invoke(folly::cpo_t<doSomething>, const SomeClass& x);
//   };
//
//  See <folly/functional/Invoke.h> for more details.
template <const auto& Tag>
using cpo_t = std::decay_t<decltype(Tag)>;

} // namespace folly
