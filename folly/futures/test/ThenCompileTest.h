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

#include <memory>

#include <folly/futures/Future.h>
#include <folly/portability/GTest.h>

namespace folly {

using A = std::unique_ptr<int>;
struct B {};

template <class T>
Future<T> someFuture() {
  return makeFuture(T());
}

template <class Ret>
Ret makeRet() {
  if constexpr (isFuture<Ret>::value) {
    using T = typename Ret::value_type;
    return makeFuture(T());
  } else {
    return Ret();
  }
}

template <class Ret, class... Params>
Ret aFunction(Params...) {
  return makeRet<Ret>();
}

template <class Ret, class... Params>
std::function<Ret(Params...)> aStdFunction() {
  return [](Params...) -> Ret { return makeRet<Ret>(); };
}

class SomeClass {
 public:
  template <class Ret, class... Params>
  static Ret aStaticMethod(Params...) {
    return makeRet<Ret>();
  }

  template <class Ret, class... Params>
  Ret aMethod(Params...) {
    return makeRet<Ret>();
  }
};

} // namespace folly
