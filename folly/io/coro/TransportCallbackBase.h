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

#include <folly/coro/Baton.h>
#include <folly/coro/Task.h>
#include <folly/io/async/AsyncTransport.h>
#if FOLLY_HAVE_OPENSSL
#include <folly/io/async/ssl/SSLErrors.h>
#endif

#if FOLLY_HAS_COROUTINES

namespace folly {
namespace coro {

//
// Common base for all callbcaks
//

class TransportCallbackBase {
 public:
  explicit TransportCallbackBase(folly::AsyncTransport& transport)
      : transport_{transport} {}

  virtual ~TransportCallbackBase() noexcept = default;

  folly::exception_wrapper& error() noexcept { return error_; }
  void post() noexcept { baton_.post(); }
  Task<folly::Unit> wait() {
    auto cancelToken = co_await co_current_cancellation_token;
    if (cancelToken.isCancellationRequested()) {
      cancel();
      co_yield folly::coro::co_stopped_may_throw;
    }
    folly::CancellationCallback cancellationCallback{
        cancelToken, [this] {
          this->post();
          VLOG(5) << "Cancellation was called";
        }};

    co_await baton_;
    VLOG(5) << "After baton await";

    if (cancelToken.isCancellationRequested()) {
      cancel();
      co_yield folly::coro::co_stopped_may_throw;
    }
    co_return folly::unit;
  }

 protected:
  // we use this to notify the other side of completion
  Baton baton_;
  // needed to modify AsyncTransport state, e.g. cacncel callbacks
  folly::AsyncTransport& transport_;

  // to wrap AsyncTransport errors
  folly::exception_wrapper error_;

  void storeException(const folly::AsyncSocketException& ex) {
#if FOLLY_HAVE_OPENSSL
    if (auto sslErr = dynamic_cast<const folly::SSLException*>(&ex)) {
      error_ = folly::make_exception_wrapper<folly::SSLException>(*sslErr);
      return;
    }
#endif
    error_ = folly::make_exception_wrapper<folly::AsyncSocketException>(ex);
  }

 private:
  virtual void cancel() noexcept = 0;
};

} // namespace coro
} // namespace folly

#endif // FOLLY_HAS_COROUTINES
