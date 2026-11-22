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

#include <folly/futures/detail/Core.h>

#include <new>

#include <fmt/core.h>
#include <folly/Utility.h>
#include <folly/futures/detail/Types.h>
#include <folly/lang/Assume.h>

namespace folly {
namespace futures {
namespace detail {

namespace {

template <class Enum>
void terminate_unexpected_state(fmt::string_view context, Enum state) {
  terminate_with<std::logic_error>(
      fmt::format("{} unexpected state: {}", context, to_underlying(state)));
}

} // namespace

void UniqueDeleter::operator()(DeferredExecutor* ptr) {
  if (ptr) {
    ptr->release();
  }
}

KeepAliveOrDeferred::KeepAliveOrDeferred(KeepAliveOrDeferred&& other) noexcept
    : state_(other.state_) {
  switch (state_) {
    case State::Deferred:
      ::new (&deferred_) DW{std::move(other.deferred_)};
      break;
    case State::KeepAlive:
      ::new (&keepAlive_) KA{std::move(other.keepAlive_)};
      break;
  }
}

KeepAliveOrDeferred::~KeepAliveOrDeferred() {
  switch (state_) {
    case State::Deferred:
      deferred_.~DW();
      break;
    case State::KeepAlive:
      keepAlive_.~KA();
      break;
  }
}

KeepAliveOrDeferred& KeepAliveOrDeferred::operator=(
    KeepAliveOrDeferred&& other) noexcept {
  // This is safe to do because KeepAliveOrDeferred is nothrow
  // move-constructible.
  this->~KeepAliveOrDeferred();
  ::new (this) KeepAliveOrDeferred{std::move(other)};
  return *this;
}

DeferredExecutor* KeepAliveOrDeferred::getDeferredExecutor() const noexcept {
  switch (state_) {
    case State::Deferred:
      return deferred_.get();
    case State::KeepAlive:
      return nullptr;
  }
  assume_unreachable();
}

Executor* KeepAliveOrDeferred::getKeepAliveExecutor() const noexcept {
  switch (state_) {
    case State::Deferred:
      return nullptr;
    case State::KeepAlive:
      return keepAlive_.get();
  }
  assume_unreachable();
}

KeepAliveOrDeferred::KA KeepAliveOrDeferred::stealKeepAlive() && noexcept {
  switch (state_) {
    case State::Deferred:
      return KA{};
    case State::KeepAlive:
      return std::move(keepAlive_);
  }
  assume_unreachable();
}

KeepAliveOrDeferred::DW KeepAliveOrDeferred::stealDeferred() && noexcept {
  switch (state_) {
    case State::Deferred:
      return std::move(deferred_);
    case State::KeepAlive:
      return DW{};
  }
  assume_unreachable();
}

KeepAliveOrDeferred KeepAliveOrDeferred::copy() const {
  switch (state_) {
    case State::Deferred:
      if (auto def = getDeferredExecutor()) {
        return KeepAliveOrDeferred{def->copy()};
      } else {
        return KeepAliveOrDeferred{};
      }
    case State::KeepAlive:
      return KeepAliveOrDeferred{keepAlive_};
  }
  assume_unreachable();
}

/* explicit */ KeepAliveOrDeferred::operator bool() const noexcept {
  return getDeferredExecutor() || getKeepAliveExecutor();
}

void DeferredExecutor::addFrom(
    Executor::KeepAlive<>&& completingKA,
    Executor::KeepAlive<>::KeepAliveFunc func) {
  auto state = state_.load(std::memory_order_acquire);
  if (state == State::DETACHED) {
    return;
  }

  // If we are completing on the current executor, call inline, otherwise
  // add
  auto addWithInline =
      [&](Executor::KeepAlive<>::KeepAliveFunc&& addFunc) mutable {
        if (completingKA.get() == executor_.get()) {
          addFunc(std::move(completingKA));
        } else {
          executor_.copy().add(std::move(addFunc));
        }
      };

  if (state == State::HAS_EXECUTOR) {
    addWithInline(std::move(func));
    return;
  }
  if (state != State::EMPTY) {
    terminate_unexpected_state("DeferredExecutor::addFrom", state);
  }
  func_ = std::move(func);
  if (folly::atomic_compare_exchange_strong_explicit(
          &state_,
          &state,
          State::HAS_FUNCTION,
          std::memory_order_release,
          std::memory_order_acquire)) {
    return;
  }

  if (state == State::DETACHED) {
    std::exchange(func_, nullptr);
  } else if (state == State::HAS_EXECUTOR) {
    addWithInline(std::exchange(func_, nullptr));
  } else {
    terminate_unexpected_state("DeferredExecutor::addFrom", state);
  }
}

Executor* DeferredExecutor::getExecutor() const {
  assert(executor_.get());
  return executor_.get();
}

void DeferredExecutor::setExecutor(
    folly::Executor::KeepAlive<> executor, bool inlineUnsafe) {
  if (nestedExecutors_) {
    auto nestedExecutors = std::exchange(nestedExecutors_, nullptr);
    for (auto& nestedExecutor : *nestedExecutors) {
      assert(nestedExecutor.get());
      nestedExecutor.get()->setExecutor(executor.copy());
    }
  }
  executor_ = std::move(executor);
  auto state = state_.load(std::memory_order_acquire);
  if (state == State::EMPTY &&
      folly::atomic_compare_exchange_strong_explicit(
          &state_,
          &state,
          State::HAS_EXECUTOR,
          std::memory_order_release,
          std::memory_order_acquire)) {
    return;
  }

  if (state != State::HAS_FUNCTION ||
      !folly::atomic_compare_exchange_strong_explicit(
          &state_,
          &state,
          State::HAS_EXECUTOR,
          std::memory_order_release,
          std::memory_order_relaxed)) {
    terminate_unexpected_state("DeferredExecutor::setExecutor", state);
  }
  if (inlineUnsafe) {
    std::exchange(func_, nullptr)(executor_.copy());
  } else {
    executor_.copy().add(std::exchange(func_, nullptr));
  }
}

void DeferredExecutor::setNestedExecutors(
    std::vector<DeferredWrapper> executors) {
  DCHECK(!nestedExecutors_);
  nestedExecutors_ =
      std::make_unique<std::vector<DeferredWrapper>>(std::move(executors));
}

void DeferredExecutor::detach() {
  if (nestedExecutors_) {
    auto nestedExecutors = std::exchange(nestedExecutors_, nullptr);
    for (auto& nestedExecutor : *nestedExecutors) {
      assert(nestedExecutor.get());
      nestedExecutor.get()->detach();
    }
  }
  auto state = state_.load(std::memory_order_acquire);
  if (state == State::EMPTY &&
      folly::atomic_compare_exchange_strong_explicit(
          &state_,
          &state,
          State::DETACHED,
          std::memory_order_release,
          std::memory_order_acquire)) {
    return;
  }

  if (state != State::HAS_FUNCTION ||
      !folly::atomic_compare_exchange_strong_explicit(
          &state_,
          &state,
          State::DETACHED,
          std::memory_order_release,
          std::memory_order_relaxed)) {
    terminate_unexpected_state("DeferredExecutor::detach", state);
  }
  std::exchange(func_, nullptr);
}

DeferredWrapper DeferredExecutor::copy() {
  acquire();
  return DeferredWrapper(this);
}

/* static */ DeferredWrapper DeferredExecutor::create() {
  return DeferredWrapper(new DeferredExecutor{});
}

DeferredExecutor::DeferredExecutor() {}

void DeferredExecutor::acquire() {
  auto keepAliveCount = keepAliveCount_.fetch_add(1, std::memory_order_relaxed);
  DCHECK_GT(keepAliveCount, 0);
}

void DeferredExecutor::release() {
  auto keepAliveCount = keepAliveCount_.fetch_sub(1, std::memory_order_acq_rel);
  DCHECK_GT(keepAliveCount, 0);
  if (keepAliveCount == 1) {
    delete this;
  }
}

InterruptHandler::~InterruptHandler() = default;

void InterruptHandler::acquire() {
  auto refCount = refCount_.fetch_add(1, std::memory_order_relaxed);
  DCHECK_GT(refCount, 0);
}

void InterruptHandler::release() {
  auto refCount = refCount_.fetch_sub(1, std::memory_order_acq_rel);
  DCHECK_GT(refCount, 0);
  if (refCount == 1) {
    delete this;
  }
}

bool CoreBase::hasResult() const noexcept {
  constexpr auto allowed = State::OnlyResult | State::Done;
  auto core = this;
  auto state = core->state_.load(std::memory_order_acquire);
  while (state == State::Proxy) {
    core = core->proxy_;
    state = core->state_.load(std::memory_order_acquire);
  }
  return State() != (state & allowed);
}

DeferredWrapper CoreBase::stealDeferredExecutor() {
  if (executor_.isKeepAlive()) {
    return {};
  }

  return std::move(executor_).stealDeferred();
}

void CoreBase::raise(exception_wrapper e) {
  if (hasResult()) {
    return;
  }
  auto interrupt = interrupt_.load(std::memory_order_acquire);
  switch (interrupt & InterruptMask) {
    case InterruptInitial: { // store the object
      assert(!interrupt);
      auto object = new exception_wrapper(std::move(e));
      auto exchanged = folly::atomic_compare_exchange_strong_explicit(
          &interrupt_,
          &interrupt,
          reinterpret_cast<uintptr_t>(object) | InterruptHasObject,
          std::memory_order_release,
          std::memory_order_acquire);
      if (exchanged) {
        return;
      }
      // lost the race!
      e = std::move(*object);
      delete object;
      if (interrupt & InterruptHasObject) { // ignore all calls after the first
        return;
      }
      assert(interrupt & InterruptHasHandler);
      [[fallthrough]];
    }
    case InterruptHasHandler: { // invoke the stored handler
      auto pointer = interrupt & ~InterruptMask;
      auto exchanged = interrupt_.compare_exchange_strong(
          interrupt, pointer | InterruptTerminal, std::memory_order_relaxed);
      if (!exchanged) { // ignore all calls after the first
        return;
      }
      auto handler = reinterpret_cast<InterruptHandler*>(pointer);
      handler->handle(e);
      return;
    }
    case InterruptHasObject: // ignore all calls after the first
      return;
    case InterruptTerminal: // ignore all calls after the first
      return;
  }
}

void CoreBase::initCopyInterruptHandlerFrom(const CoreBase& other) {
  assert(!interrupt_.load(std::memory_order_relaxed));
  auto interrupt = other.interrupt_.load(std::memory_order_acquire);
  switch (interrupt & InterruptMask) {
    case InterruptHasHandler: { // copy the handler
      auto pointer = interrupt & ~InterruptMask;
      auto handler = reinterpret_cast<InterruptHandler*>(pointer);
      handler->acquire();
      interrupt_.store(
          pointer | InterruptHasHandler, std::memory_order_release);
      break;
    }
    case InterruptTerminal: { // copy the handler, if any
      auto pointer = interrupt & ~InterruptMask;
      auto handler = reinterpret_cast<InterruptHandler*>(pointer);
      if (handler) {
        handler->acquire();
        interrupt_.store(
            pointer | InterruptHasHandler, std::memory_order_release);
      }
      break;
    }
  }
}

class CoreBase::CoreAndCallbackReference {
 public:
  explicit CoreAndCallbackReference(CoreBase* core) noexcept : core_(core) {}

  ~CoreAndCallbackReference() noexcept { detach(); }

  CoreAndCallbackReference(CoreAndCallbackReference const& o) = delete;
  CoreAndCallbackReference& operator=(CoreAndCallbackReference const& o) =
      delete;
  CoreAndCallbackReference& operator=(CoreAndCallbackReference&&) = delete;

  CoreAndCallbackReference(CoreAndCallbackReference&& o) noexcept
      : core_(std::exchange(o.core_, nullptr)) {}

  CoreBase* getCore() const noexcept { return core_; }

 private:
  void detach() noexcept {
    if (core_) {
      core_->derefCallback();
      core_->detachOne();
    }
  }

  CoreBase* core_{nullptr};
};

CoreBase::~CoreBase() {
  auto interrupt = interrupt_.load(std::memory_order_acquire);
  auto pointer = interrupt & ~InterruptMask;
  switch (interrupt & InterruptMask) {
    case InterruptHasHandler: {
      auto handler = reinterpret_cast<InterruptHandler*>(pointer);
      handler->release();
      break;
    }
    case InterruptHasObject: {
      auto object = reinterpret_cast<exception_wrapper*>(pointer);
      delete object;
      break;
    }
    case InterruptTerminal: {
      auto handler = reinterpret_cast<InterruptHandler*>(pointer);
      if (handler) {
        handler->release();
      }
      break;
    }
  }
}

void CoreBase::setCallback_(
    Callback&& callback,
    std::shared_ptr<folly::RequestContext>&& context,
    futures::detail::InlineContinuation allowInline) {
  DCHECK(!hasCallback());

  callback_ = std::move(callback);
  context_ = std::move(context);

  auto state = state_.load(std::memory_order_acquire);
  State nextState = allowInline == futures::detail::InlineContinuation::permit
      ? State::OnlyCallbackAllowInline
      : State::OnlyCallback;

  if (state == State::Start) {
    if (folly::atomic_compare_exchange_strong_explicit(
            &state_,
            &state,
            nextState,
            std::memory_order_release,
            std::memory_order_acquire)) {
      return;
    }
  }

  if (state == State::OnlyResult) {
    if (!folly::atomic_compare_exchange_strong_explicit(
            &state_,
            &state,
            State::Done,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      terminate_unexpected_state("setCallback", state);
    }
    auto&& completingKA =
        allowInline == futures::detail::InlineContinuation::permit
        ? executor_.getKeepAliveExecutor()
        : Executor::KeepAlive<>{};
    doCallback(std::move(completingKA), allowInline);
  } else if (state == State::Proxy) {
    if (!folly::atomic_compare_exchange_strong_explicit(
            &state_,
            &state,
            State::Empty,
            std::memory_order_relaxed,
            std::memory_order_relaxed)) {
      terminate_unexpected_state("setCallback", state);
    }
    return proxyCallback(state);
  } else {
    terminate_unexpected_state("setCallback", state);
  }
}

void CoreBase::setResult_(Executor::KeepAlive<>&& completingKA) {
  auto state = state_.load(std::memory_order_acquire);
  switch (state) {
    case State::Start:
      if (folly::atomic_compare_exchange_strong_explicit(
              &state_,
              &state,
              State::OnlyResult,
              std::memory_order_release,
              std::memory_order_acquire)) {
        return;
      }
      [[fallthrough]];

    case State::OnlyCallback:
    case State::OnlyCallbackAllowInline:
      if ((state != State::OnlyCallback &&
           state != State::OnlyCallbackAllowInline) ||
          !folly::atomic_compare_exchange_strong_explicit(
              &state_,
              &state,
              State::Done,
              std::memory_order_relaxed,
              std::memory_order_relaxed)) {
        terminate_unexpected_state("setResult", state);
      }
      doCallback(
          std::move(completingKA),
          state == State::OnlyCallbackAllowInline
              ? futures::detail::InlineContinuation::permit
              : futures::detail::InlineContinuation::forbid);
      return;
    case State::OnlyResult:
    case State::Proxy:
    case State::Done:
    case State::Empty:
    default:
      terminate_unexpected_state("setResult", state);
  }
}

void CoreBase::setProxy_(CoreBase* proxy) {
  proxy_ = proxy;

  auto state = state_.load(std::memory_order_acquire);
  switch (state) {
    case State::Start:
      if (folly::atomic_compare_exchange_strong_explicit(
              &state_,
              &state,
              State::Proxy,
              std::memory_order_release,
              std::memory_order_acquire)) {
        break;
      }
      [[fallthrough]];

    case State::OnlyCallback:
    case State::OnlyCallbackAllowInline:
      if ((state != State::OnlyCallback &&
           state != State::OnlyCallbackAllowInline) ||
          !folly::atomic_compare_exchange_strong_explicit(
              &state_,
              &state,
              State::Empty,
              std::memory_order_relaxed,
              std::memory_order_relaxed)) {
        terminate_unexpected_state("setCallback", state);
      }
      proxyCallback(state);
      break;
    case State::OnlyResult:
    case State::Proxy:
    case State::Done:
    case State::Empty:
    default:
      terminate_unexpected_state("setCallback", state);
  }

  detachOne();
}

// May be called at most once.
void CoreBase::doCallback(
    Executor::KeepAlive<>&& completingKA,
    futures::detail::InlineContinuation allowInline) {
  DCHECK(state_ == State::Done);

  auto executor = std::exchange(executor_, KeepAliveOrDeferred{});

  // Customise inline behaviour
  // If addCompletingKA is non-null, then we are allowing inline execution
  auto doAdd =
      [](Executor::KeepAlive<>&& addCompletingKA,
         KeepAliveOrDeferred&& currentExecutor,
         auto&& keepAliveFunc) mutable {
        if (auto deferredExecutorPtr = currentExecutor.getDeferredExecutor()) {
          deferredExecutorPtr->addFrom(
              std::move(addCompletingKA), std::move(keepAliveFunc));
        } else {
          // If executors match call inline
          auto currentKeepAlive = std::move(currentExecutor).stealKeepAlive();
          if (addCompletingKA.get() == currentKeepAlive.get()) {
            keepAliveFunc(std::move(currentKeepAlive));
          } else {
            std::move(currentKeepAlive).add(std::move(keepAliveFunc));
          }
        }
      };

  if (executor) {
    // If we are not allowing inline, clear the completing KA to disallow
    if (allowInline == futures::detail::InlineContinuation::forbid) {
      completingKA = Executor::KeepAlive<>{};
    }
    exception_wrapper ew;
    // We need to reset `callback_` after it was executed (which can happen
    // through the executor or, if `Executor::add` throws, below). The
    // executor might discard the function without executing it (now or
    // later), in which case `callback_` also needs to be reset.
    // The `Core` has to be kept alive throughout that time, too. Hence we
    // increment `attached_` and `callbackReferences_` by two, and construct
    // exactly two `CoreAndCallbackReference` objects, which call
    // `derefCallback` and `detachOne` in their destructor. One will guard
    // this scope, the other one will guard the lambda passed to the executor.
    attached_.fetch_add(2, std::memory_order_relaxed);
    callbackReferences_.fetch_add(2, std::memory_order_relaxed);
    CoreAndCallbackReference guard_local_scope(this);
    CoreAndCallbackReference guard_lambda(this);
    try {
      doAdd(
          std::move(completingKA),
          std::move(executor),
          [core_ref =
               std::move(guard_lambda)](Executor::KeepAlive<>&& ka) mutable {
            auto cr = std::move(core_ref);
            CoreBase* const core = cr.getCore();
            RequestContextScopeGuard rctx(std::move(core->context_));
            core->callback_(*core, std::move(ka), nullptr);
          });
    } catch (...) {
      ew = exception_wrapper(current_exception());
    }
    if (ew) {
      RequestContextScopeGuard rctx(std::move(context_));
      callback_(*this, Executor::KeepAlive<>{}, &ew);
    }
  } else {
    attached_.fetch_add(1, std::memory_order_relaxed);
    SCOPE_EXIT {
      context_ = {};
      callback_ = {};
      detachOne();
    };
    RequestContextScopeGuard rctx(std::move(context_));
    callback_(*this, std::move(completingKA), nullptr);
  }
}

void CoreBase::proxyCallback(State priorState) {
  // If the state of the core being proxied had a callback that allows inline
  // execution, maintain this information in the proxy
  futures::detail::InlineContinuation allowInline =
      (priorState == State::OnlyCallbackAllowInline
           ? futures::detail::InlineContinuation::permit
           : futures::detail::InlineContinuation::forbid);
  proxy_->setExecutor(std::move(executor_));
  proxy_->setCallback_(std::move(callback_), std::move(context_), allowInline);
  proxy_->detachFuture();
  context_ = {};
  callback_ = {};
}

void CoreBase::detachOne() noexcept {
  auto a = attached_.fetch_sub(1, std::memory_order_acq_rel);
  assert(a >= 1);
  if (a == 1) {
    delete this;
  }
}

void CoreBase::derefCallback() noexcept {
  auto c = callbackReferences_.fetch_sub(1, std::memory_order_acq_rel);
  assert(c >= 1);
  if (c == 1) {
    context_ = {};
    callback_ = {};
  }
}

bool CoreBase::destroyDerived() noexcept {
  DCHECK(attached_ == 0);
  auto state = state_.load(std::memory_order_relaxed);
  switch (state) {
    case State::OnlyResult:
      [[fallthrough]];

    case State::Done:
      return true;

    case State::Proxy:
      proxy_->detachFuture();
      [[fallthrough]];

    case State::Empty:
      return false;

    case State::Start:
    case State::OnlyCallback:
    case State::OnlyCallbackAllowInline:
    default:
      terminate_with<std::logic_error>("~Core unexpected state");
  }
}

#if FOLLY_USE_EXTERN_FUTURE_UNIT
template class Core<folly::Unit>;
#endif

} // namespace detail
} // namespace futures
} // namespace folly
