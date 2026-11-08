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

#include <folly/futures/Future.h>

#include <folly/Likely.h>
#include <folly/Singleton.h>
#include <folly/futures/HeapTimekeeper.h>
// ThreadWheelTimekeeper runs an EventBase, which needs libevent and pipe(),
// which wasi lacks.
#if !defined(__wasi__)
#include <folly/futures/ThreadWheelTimekeeper.h>
#include <folly/portability/GFlags.h>

FOLLY_GFLAGS_DEFINE_bool(
    folly_futures_use_thread_wheel_timekeeper,
    false,
    "Use ThreadWheelTimekeeper for the default Future timekeeper singleton");
#endif

namespace folly {
namespace futures {

namespace detail {

void WithinInterruptHandler::operator()(exception_wrapper const& ew) const {
  if (auto locked = ptr.lock()) {
    locked->raise(ew);
  }
}

void WithinAfterFutureCallback::operator()(Try<Unit>&& t) {
  if (t.hasException() &&
      t.exception().is_compatible_with<FutureCancellation>()) {
    // This got cancelled by thisFuture so we can just return.
    return;
  }

  auto lockedCtx = ctx.lock();
  if (!lockedCtx) {
    // ctx already released. "this" completed first, cancel "after"
    return;
  }
  // "after" completed first, cancel "this"
  lockedCtx->thisFuture.raise(FutureTimeout());
  if (!lockedCtx->token.exchange(true, std::memory_order_relaxed)) {
    auto& exn = t.hasException() ? t.exception() : lockedCtx->exception;
    lockedCtx->doPromiseSetException(*lockedCtx, std::move(exn));
  }
}

} // namespace detail

SemiFuture<Unit> sleep(HighResDuration dur, Timekeeper* tk) {
  std::shared_ptr<Timekeeper> tks;
  if (!tk) [[likely]] {
    tks = folly::detail::getTimekeeperSingleton();
    tk = tks.get();
  }

  if (!tk) [[unlikely]] {
    return makeSemiFuture<Unit>(FutureNoTimekeeper());
  }

  return tk->after(dur);
}

Future<Unit> sleepUnsafe(HighResDuration dur, Timekeeper* tk) {
  return sleep(dur, tk).toUnsafeFuture();
}

#if !defined(__wasi__)
namespace {
template <typename Ptr>
class FutureWaiter : public fibers::Baton::Waiter {
 public:
  FutureWaiter(Promise<Unit> promise, Ptr baton)
      : promise_(std::move(promise)), baton_(std::move(baton)) {
    baton_->setWaiter(*this);
  }

  void post() override {
    promise_.setValue();
    delete this;
  }

 private:
  Promise<Unit> promise_;
  Ptr baton_;
};
} // namespace

SemiFuture<Unit> wait(std::unique_ptr<fibers::Baton> baton) {
  Promise<Unit> promise;
  auto sf = promise.getSemiFuture();
  new FutureWaiter<std::unique_ptr<fibers::Baton>>(
      std::move(promise), std::move(baton));
  return sf;
}
SemiFuture<Unit> wait(std::shared_ptr<fibers::Baton> baton) {
  Promise<Unit> promise;
  auto sf = promise.getSemiFuture();
  new FutureWaiter<std::shared_ptr<fibers::Baton>>(
      std::move(promise), std::move(baton));
  return sf;
}
#endif

} // namespace futures

namespace detail {

FOLLY_EXPORT FOLLY_ALWAYS_INLINE
    folly::Singleton<Timekeeper, TimekeeperSingletonTag>&
    getOrCreateTimekeeperSingleton() {
  using TimekeeperSingleton =
      folly::Singleton<Timekeeper, TimekeeperSingletonTag>;
  static TimekeeperSingleton sTimekeeperSingleton([]() -> Timekeeper* {
#if !defined(__wasi__)
    if (FLAGS_folly_futures_use_thread_wheel_timekeeper) {
      return new ThreadWheelTimekeeper;
    }
#endif
    return new HeapTimekeeper;
  });
  return sTimekeeperSingleton;
}

namespace {
class TimekeeperSingletonInstantiator {
 public:
  TimekeeperSingletonInstantiator() {
    // call this function to force the timekeeper singleton to be created
    folly::detail::getOrCreateTimekeeperSingleton();
  }
};
TimekeeperSingletonInstantiator gTimekeeperSingletonInstantiator;
} // namespace

std::shared_ptr<Timekeeper> getTimekeeperSingleton() {
  (void)gTimekeeperSingletonInstantiator;
  return getOrCreateTimekeeperSingleton().try_get();
}

} // namespace detail

#if FOLLY_USE_EXTERN_FUTURE_UNIT
namespace futures {
namespace detail {
template class FutureBase<Unit>;
} // namespace detail
} // namespace futures

template class Future<Unit>;
template class SemiFuture<Unit>;
#endif

} // namespace folly
