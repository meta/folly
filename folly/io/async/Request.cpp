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

#include <folly/io/async/Request.h>

#include <stdexcept>

#include <thread>

#include <folly/Demangle.h>
#include <folly/GLog.h>
#include <folly/ScopeGuard.h>
#include <folly/Utility.h>
#include <folly/concurrency/container/SingleWriterFixedHashMap.h>
#include <folly/tracing/StaticTracepoint.h>

namespace folly {
// Thread-local cache of raw RequestContext.
// Updated by setContext() and setShallowCopyContext() to avoid calling
// SingletonThreadLocal::get() in locations requiring async-signal-safe access.
thread_local RequestContext* gAsyncSignalSafeRequestContextCache = nullptr;

RequestContext* getCachedRequestContext() {
  return gAsyncSignalSafeRequestContextCache;
}

RequestToken::RequestToken(const std::string& str) {
  auto& cache = getCache();
  {
    auto c = cache.rlock();
    auto res = c->find(str);
    if (res != c->end()) {
      token_ = res->second;
      return;
    }
  }
  auto c = cache.wlock();
  auto res = c->find(str);
  if (res != c->end()) {
    token_ = res->second;
    return;
  }
  static uint32_t nextToken{1};

  token_ = nextToken++;
  (*c)[str] = token_;
}

std::string RequestToken::getDebugString() const {
  auto& cache = getCache();
  auto c = cache.rlock();
  for (auto& v : *c) {
    if (v.second == token_) {
      return v.first;
    }
  }
  throw std::logic_error("Could not find debug string in RequestToken");
}

Synchronized<F14FastMap<std::string, uint32_t>>& RequestToken::getCache() {
  static Indestructible<Synchronized<F14FastMap<std::string, uint32_t>>> cache;
  return *cache;
}

FOLLY_ALWAYS_INLINE
void RequestData::acquireRef() {
  auto rc = keepAliveCounter_.fetch_add(
      kClearDeleteCounts, std::memory_order_relaxed);
  DCHECK_GE(rc, 0);
}

void RequestData::releaseRefClearOnly() {
  auto rc =
      keepAliveCounter_.fetch_sub(kClearCount, std::memory_order_acq_rel) -
      kClearCount;
  DCHECK_GT(rc, 0);
  if (rc < kClearCount) {
    this->onClear();
  }
}

void RequestData::releaseRefDeleteOnly() {
  auto rc =
      keepAliveCounter_.fetch_sub(kDeleteCount, std::memory_order_acq_rel) -
      kDeleteCount;
  DCHECK_GE(rc, 0);
  if (rc == 0) {
    delete this;
  }
}

FOLLY_ALWAYS_INLINE
void RequestData::releaseRefClearDelete() {
  auto rc = keepAliveCounter_.load(std::memory_order_acquire);
  if (FOLLY_LIKELY(rc == kClearDeleteCounts)) {
    this->onClear();
    delete this;
    return;
  }
  // After decrementing the delete count, another thread could be left
  // with the last reference and delete the object, so after the
  // operation we cannot safely access the object. In particular we
  // cannot call `onClear()` on it, so we only combine the decrements
  // if the clear count is left positive, so that we are not
  // responsible for invoking the callback.
  if (rc - kClearDeleteCounts >= kClearCount &&
      keepAliveCounter_.compare_exchange_strong(
          rc,
          rc - kClearDeleteCounts,
          std::memory_order_release,
          std::memory_order_relaxed)) {
    return;
  }
  releaseRefClearDeleteSlow();
}

FOLLY_NOINLINE
void RequestData::releaseRefClearDeleteSlow() {
  releaseRefClearOnly();
  releaseRefDeleteOnly();
}

// The Combined struct keeps the two structures for context data
// and callbacks together, so that readers can protect consistent
// versions of the two structures together using hazard pointers.
struct RequestContext::State::Combined : hazptr_obj_base<Combined> {
  static constexpr size_t kInitialCapacity = 4;
  static constexpr size_t kSlackReciprocal = 4; // unused >= 1/4 capacity

  // This must be optimized for lookup, its hot path is getContextData
  // Efficiency of copying the container also matters in setShallowCopyContext
  SingleWriterFixedHashMap<RequestToken, RequestData*> requestData_;
  // This must be optimized for iteration, its hot path is setContext
  SingleWriterFixedHashMap<RequestData*, int> callbackData_;
  // Vector of cleared data. Accessed only sequentially by writers.
  std::vector<std::pair<RequestToken, RequestData*>> cleared_;

  Combined()
      : requestData_(kInitialCapacity), callbackData_(kInitialCapacity) {}

  Combined(const Combined& o)
      : Combined(o.requestData_.capacity(), o.callbackData_.capacity(), o) {}

  Combined(size_t dataCapacity, size_t callbackCapacity, const Combined& o)
      : requestData_(dataCapacity, o.requestData_),
        callbackData_(callbackCapacity, o.callbackData_) {}

  Combined(Combined&&) = delete;
  Combined& operator=(const Combined&) = delete;
  Combined& operator=(Combined&&) = delete;

  ~Combined() { releaseDataRefs(); }

  /* acquireDataRefs - Called at most once per Combined instance. */
  void acquireDataRefs() {
    for (auto it = requestData_.begin(); it != requestData_.end(); ++it) {
      auto p = it.value();
      if (p) {
        p->acquireRef();
      }
    }
  }

  /* releaseDataRefs - Called only once from ~Combined */
  void releaseDataRefs() {
    if (!cleared_.empty()) {
      for (auto& pair : cleared_) {
        pair.second->releaseRefDeleteOnly();
        requestData_.erase(pair.first);
      }
    }
    for (auto it = requestData_.begin(); it != requestData_.end(); ++it) {
      RequestData* data = it.value();
      if (data) {
        data->releaseRefClearDelete();
      }
    }
  }

  void debugCheckConsistency() {
    if constexpr (kIsDebug) {
      size_t numRequestData = 0;
      size_t numHasCallback = 0;
      for (auto it = requestData_.begin(); it != requestData_.end(); ++it) {
        ++numRequestData;
        if (auto* p = it.value(); p && p->hasCallback()) {
          ++numHasCallback;
          CHECK(callbackData_.contains(p))
              << folly::demangle(typeid(*p)) << " "
              << it.key().getDebugString();
        }
      }
      CHECK_EQ(numRequestData, requestData_.size());
      size_t numCallbackData = 0;
      for (auto it = callbackData_.begin(); it != callbackData_.end(); ++it) {
        ++numCallbackData;
        auto* p = it.key();
        CHECK(p);
        CHECK(p->hasCallback()) << folly::demangle(typeid(*p));
      }
      CHECK_EQ(numHasCallback, numCallbackData);
      CHECK_EQ(numCallbackData, callbackData_.size());
    }
  }

  /* needExpand */
  bool needExpand() {
    return needExpandRequestData() || needExpandCallbackData();
  }

  /* needExpandRequestData */
  bool needExpandRequestData() {
    return kSlackReciprocal * (requestData_.available() - 1) <
        requestData_.capacity();
  }

  /* needExpandCallbackData */
  bool needExpandCallbackData() {
    return kSlackReciprocal * (callbackData_.available() - 1) <
        callbackData_.capacity();
  }
}; // Combined

RequestContext::State::State() = default;

FOLLY_ALWAYS_INLINE
RequestContext::State::State(const State& o) {
  // Even though Combined's maps can be individually iterated without
  // synchronization, we need a consistent snapshot, so we have to synchronize
  // with writers.
  std::shared_lock lock(o.mutex_);
  Combined* oc = o.combined();
  if (oc) {
    auto p = new Combined(*oc);
    p->debugCheckConsistency();
    p->acquireDataRefs();
    setCombined(p);
  }
}

RequestContext::State::~State() {
  cohort_.shutdown_and_reclaim();
  auto p = combined();
  if (p) {
    delete p;
  }
}

class [[nodiscard]] RequestContext::State::VersionGuard {
 public:
  explicit VersionGuard(RequestContext::State& state) : state_(state) {}

  ~VersionGuard() {
    state_.version_.store(processLocalUniqueId(), std::memory_order_release);
  }

 private:
  VersionGuard(const VersionGuard&) = delete;
  VersionGuard(VersionGuard&&) = delete;
  VersionGuard& operator=(const VersionGuard&) = delete;
  VersionGuard& operator=(VersionGuard&&) = delete;

  RequestContext::State& state_;
};

class [[nodiscard]] RequestContext::State::LockGuard {
 public:
  explicit LockGuard(RequestContext::State& state)
      : state_(state), lock_(state.mutex_) {}

  ~LockGuard() {
    // Publish while holding the lock so versions follow mutex order. Otherwise
    // an earlier writer could publish after a later writer has completed.
    state_.version_.store(processLocalUniqueId(), std::memory_order_release);
  }

 private:
  LockGuard(const LockGuard&) = delete;
  LockGuard(LockGuard&&) = delete;
  LockGuard& operator=(const LockGuard&) = delete;
  LockGuard& operator=(LockGuard&&) = delete;

  RequestContext::State& state_;
  std::unique_lock<folly::SharedMutex> lock_;
};

FOLLY_ALWAYS_INLINE
RequestContext::State::Combined* RequestContext::State::combined() const {
  return combined_.load(std::memory_order_acquire);
}

FOLLY_ALWAYS_INLINE
RequestContext::State::Combined* RequestContext::State::ensureCombined() {
  auto c = combined();
  if (!c) {
    c = new Combined;
    setCombined(c);
  }
  return c;
}

FOLLY_ALWAYS_INLINE
void RequestContext::State::setCombined(Combined* p) {
  p->set_cohort_tag(&cohort_);
  combined_.store(p, std::memory_order_release);
}

FOLLY_ALWAYS_INLINE
bool RequestContext::State::doSetContextData(
    const RequestToken& token,
    std::unique_ptr<RequestData>& data,
    DoSetBehaviour behaviour,
    bool safe) {
  SetContextDataResult result;
  if (safe) {
    VersionGuard versionGuard{*this};
    result = doSetContextDataHelper(token, data, behaviour, safe);
  } else {
    LockGuard lock{*this};
    result = doSetContextDataHelper(token, data, behaviour, safe);
  }
  if (result.unexpected) {
    FB_LOG_ONCE(WARNING) << "Calling RequestContext::setContextData for "
                         << token.getDebugString() << " but it is already set";
  }
  if (result.replaced) {
    result.replaced->retire(); // Retire to hazptr library
  }
  return result.changed;
}

FOLLY_ALWAYS_INLINE
RequestContext::State::SetContextDataResult
RequestContext::State::doSetContextDataHelper(
    const RequestToken& token,
    std::unique_ptr<RequestData>& data,
    DoSetBehaviour behaviour,
    bool safe) {
  bool unexpected = false;
  Combined* cur = ensureCombined();
  Combined* replaced = nullptr;
  auto it = cur->requestData_.find(token);
  bool found = it != cur->requestData_.end();
  if (found) {
    if (behaviour == DoSetBehaviour::SET_IF_ABSENT) {
      return {
          false /* no changes made */,
          false /* nothing unexpected */,
          nullptr /* combined not replaced */};
    }
    RequestData* oldData = it.value();
    // Always erase old data (and run onUnset callback, if any).
    // Old data will always be overwritten either by the new data
    // (if behavior is OVERWRITE) or by nullptr (if behavior is SET).
    Combined* newCombined = eraseOldData(cur, token, oldData, safe);
    DCHECK(oldData != nullptr || newCombined == nullptr);
    if (newCombined) {
      replaced = cur;
      cur = newCombined;
    }
    if (behaviour == DoSetBehaviour::SET) {
      // The expected behavior for SET when found is to reset the
      // pointer and warn, without updating to the new data.
      bool inserted = cur->requestData_.insert(token, nullptr);
      DCHECK(inserted);
      unexpected = true;
    } else {
      DCHECK(behaviour == DoSetBehaviour::OVERWRITE);
    }
  }
  if (!unexpected) {
    // Replace combined if needed, call onSet if any, insert new data.
    Combined* newCombined = insertNewData(cur, token, data, found);
    if (newCombined) {
      if (replaced) {
        delete cur;
      } else {
        replaced = cur;
      }
      cur = newCombined;
    }
  }
  if (replaced) {
    // Now the new Combined is consistent. Safe to publish.
    setCombined(cur);
  }
  return {
      true, /* changes were made */
      unexpected,
      replaced};
}

namespace {
[[noreturn]] FOLLY_NOINLINE void throwNonClearable(const RequestToken& token) {
  throw std::invalid_argument(
      "Cannot clear or overwrite non-clearable RequestData for token " +
      token.getDebugString());
}
} // namespace

FOLLY_ALWAYS_INLINE
RequestContext::State::Combined* FOLLY_NULLABLE
RequestContext::State::eraseOldData(
    RequestContext::State::Combined* cur,
    const RequestToken& token,
    RequestData* olddata,
    bool safe) {
  // Non-clearable data can only be removed from a context that is not shared.
  if (olddata && !safe && !olddata->clearable()) {
    throwNonClearable(token);
  }
  Combined* newCombined = nullptr;
  // Call onUnset, if any.
  if (olddata && olddata->hasCallback()) {
    olddata->onUnset();
    bool erased = cur->callbackData_.erase(olddata);
    DCHECK(erased);
  }
  if (safe || olddata == nullptr) {
    // If the caller guarantees thread-safety or the old data is null,
    // then erase the entry in the current version.
    bool erased = cur->requestData_.erase(token);
    DCHECK(erased);
    if (olddata) {
      olddata->releaseRefClearDelete();
    }
  } else {
    // If there may be concurrent readers, then copy-on-erase.
    // Update the data reference counts to account for the
    // existence of the new copy.
    newCombined = new Combined(*cur);
    bool erased = newCombined->requestData_.erase(token);
    DCHECK(erased);
    newCombined->acquireDataRefs();
  }
  return newCombined;
}

FOLLY_ALWAYS_INLINE
RequestContext::State::Combined* FOLLY_NULLABLE
RequestContext::State::insertNewData(
    RequestContext::State::Combined* cur,
    const RequestToken& token,
    std::unique_ptr<RequestData>& data,
    bool tokenWasPresent) {
  Combined* newCombined = nullptr;
  const bool willInsertCallbackData = data && data->hasCallback();
  // We can skip the needExpand() check if the token insertion does
  // not need expansion since it was just erased, so it can take the
  // same slot, and we're not inserting into callbackData.
  if ((!tokenWasPresent || willInsertCallbackData) && cur->needExpand()) {
    newCombined = expand(cur);
    cur = newCombined;
    cur->acquireDataRefs();
  }
  if (data && data->hasCallback()) {
    // If data has callback, insert in callback structure, call onSet
    bool inserted = cur->callbackData_.insert(data.get(), true);
    DCHECK(inserted);
    data->onSet();
  }
  if (data) {
    data->acquireRef();
  }
  bool inserted = cur->requestData_.insert(token, data.release());
  DCHECK(inserted);
  return newCombined;
}

FOLLY_ALWAYS_INLINE
bool RequestContext::State::hasContextData(const RequestToken& token) const {
  hazptr_local<1> h;
  Combined* combined = h[0].protect(combined_);
  return combined ? combined->requestData_.contains(token) : false;
}

FOLLY_ALWAYS_INLINE
RequestData* FOLLY_NULLABLE
RequestContext::State::getContextData(const RequestToken& token) {
  hazptr_local<1> h;
  Combined* combined = h[0].protect(combined_);
  if (!combined) {
    return nullptr;
  }
  auto& reqData = combined->requestData_;
  auto it = reqData.find(token);
  return it == reqData.end() ? nullptr : it.value();
}

FOLLY_ALWAYS_INLINE
const RequestData* FOLLY_NULLABLE
RequestContext::State::getContextData(const RequestToken& token) const {
  hazptr_local<1> h;
  Combined* combined = h[0].protect(combined_);
  if (!combined) {
    return nullptr;
  }
  auto& reqData = combined->requestData_;
  auto it = reqData.find(token);
  return it == reqData.end() ? nullptr : it.value();
}

FOLLY_ALWAYS_INLINE
void RequestContext::State::onSet() {
  // Don't use hazptr_local because callback may use hazptr
  hazptr_holder<> h = make_hazard_pointer<>();
  Combined* combined = h.protect(combined_);
  if (!combined) {
    return;
  }
  auto& cb = combined->callbackData_;
  for (auto it = cb.begin(); it != cb.end(); ++it) {
    it.key()->onSet();
  }
}

FOLLY_ALWAYS_INLINE
void RequestContext::State::onUnset() {
  // Don't use hazptr_local because callback may use hazptr
  hazptr_holder<> h = make_hazard_pointer<>();
  Combined* combined = h.protect(combined_);
  if (!combined) {
    return;
  }
  auto& cb = combined->callbackData_;
  for (auto it = cb.begin(); it != cb.end(); ++it) {
    it.key()->onUnset();
  }
}

void RequestContext::State::clearContextData(const RequestToken& token) {
  RequestData* data;
  Combined* replaced = nullptr;
  { // Lock mutex_
    LockGuard lock{*this};
    Combined* cur = combined();
    if (!cur) {
      return;
    }
    auto it = cur->requestData_.find(token);
    if (it == cur->requestData_.end()) {
      return;
    }
    data = it.value();
    if (!data) {
      bool erased = cur->requestData_.erase(token);
      DCHECK(erased);
      return;
    }
    if (!data->clearable()) {
      throwNonClearable(token);
    }
    if (data->hasCallback()) {
      data->onUnset();
      bool erased = cur->callbackData_.erase(data);
      DCHECK(erased);
    }
    replaced = cur;
    cur = new Combined(*replaced);
    bool erased = cur->requestData_.erase(token);
    DCHECK(erased);
    cur->acquireDataRefs();
    setCombined(cur);
  } // Unlock mutex_
  DCHECK(data);
  data->releaseRefClearOnly();
  DCHECK(replaced);
  replaced->cleared_.emplace_back(std::make_pair(token, data));
  replaced->retire();
}

RequestContext::State::Combined* RequestContext::State::expand(
    RequestContext::State::Combined* c) {
  size_t dataCapacity = c->requestData_.capacity();
  if (c->needExpandRequestData()) {
    dataCapacity *= 2;
  }
  size_t callbackCapacity = c->callbackData_.capacity();
  if (c->needExpandCallbackData()) {
    callbackCapacity *= 2;
  }
  return new Combined(dataCapacity, callbackCapacity, *c);
}

struct RequestContextHandle::Control final : DelayedDestruction {
  struct SharedState {
    ~SharedState();
    std::unique_ptr<Control> control;
    std::shared_ptr<SharedState> evbKeepAlive;
  };

  Control() = default;
  explicit Control(intptr_t rootId) : context(rootId) {}

  void checkThread() const noexcept {
#ifndef NDEBUG
    DCHECK(thread == std::this_thread::get_id());
#endif
  }

  IntrusivePtr<Control> pin() noexcept {
    checkThread();
    return IntrusivePtr<Control>(this);
  }

  void onDelayedDestroy(bool delayed) override {
    checkThread();
    if (delayed && !getDestroyPending()) {
      return;
    }
    if (shared) {
      // CPU references can release concurrently. Transfer the EVB's final
      // pin before releasing it; the last shared reference deletes Control.
      folly::copy(std::move(shared->evbKeepAlive)); // drop
      return;
    }
    delete this;
  }

  std::shared_ptr<RequestContext> capture() noexcept(new_is_infallible_v) {
    checkThread();
    if (shared) {
      return std::shared_ptr<RequestContext>(shared->evbKeepAlive, &context);
    }
    // Allocate before transferring ownership so failure leaves the EVB owner
    // intact. The remaining operations cannot throw.
    auto captured = std::make_shared<SharedState>();
    captured->control.reset(this);
    captured->evbKeepAlive = captured;
    shared = captured.get();
    return std::shared_ptr<RequestContext>(std::move(captured), &context);
  }

  RequestContext context;
  SharedState* shared{nullptr};
#ifndef NDEBUG
  const std::thread::id thread = std::this_thread::get_id();
#endif
};

RequestContextHandle::Control::SharedState::~SharedState() = default;

RequestContextHandle::RequestContextHandle() noexcept : control_(nullptr) {}

RequestContextHandle::~RequestContextHandle() {
  reset();
}

RequestContextHandle::RequestContextHandle(
    RequestContextHandle&& other) noexcept
    : control_(std::move(other.control_)) {}

RequestContextHandle& RequestContextHandle::operator=(
    RequestContextHandle&& other) noexcept {
  if (this != &other) {
    reset();
    control_ = std::move(other.control_);
  }
  return *this;
}

RequestContext* RequestContextHandle::get() const noexcept {
  if (control_) {
    control_->checkThread();
    return &control_->context;
  }
  return nullptr;
}

void RequestContextHandle::reset() noexcept {
  if (control_) {
    control_->checkThread();
    control_.reset();
  }
}

std::shared_ptr<RequestContext> RequestContextHandle::capture() const
    noexcept(new_is_infallible_v) {
  return control_ ? control_->capture() : nullptr;
}

RequestContext::ContextState::ContextState() : borrowedContext(nullptr) {}

RequestContext::ContextState::ContextState(
    std::shared_ptr<RequestContext> context)
    : requestContext(std::move(context)), borrowedContext(nullptr) {}

RequestContext::ContextState::ContextState(
    RequestContextHandle::Control& control)
    : borrowedContext(control.pin()) {}

RequestContext::ContextState::ContextState(const RequestContextHandle& handle)
    : borrowedContext(
          handle.control_
              ? handle.control_->pin()
              : DelayedDestructionBase::IntrusivePtr<
                    RequestContextHandle::Control>(nullptr)) {}

RequestContext::ContextState::~ContextState() {
  if (auto* control = localControl()) {
    control->checkThread();
  }
}

RequestContext::ContextState::ContextState(ContextState&& other) noexcept
    : requestContext(std::move(other.requestContext)),
      borrowedContext(std::move(other.borrowedContext)) {}

RequestContext::ContextState& RequestContext::ContextState::operator=(
    ContextState&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (!localControl() && !other.localControl()) {
    requestContext = std::move(other.requestContext);
    return *this;
  }
  ContextState previous(std::move(*this));
  requestContext = std::move(other.requestContext);
  borrowedContext = std::move(other.borrowedContext);
  return *this;
}

void RequestContext::ContextState::swap(ContextState& other) noexcept {
  requestContext.swap(other.requestContext);
  std::swap(borrowedContext, other.borrowedContext);
}

RequestContextHandle::Control* RequestContext::ContextState::localControl()
    const noexcept {
  return borrowedContext.get();
}

RequestContext* RequestContext::ContextState::get() const noexcept {
  auto* control = localControl();
  return control ? &control->context : requestContext.get();
}

std::shared_ptr<RequestContext> RequestContext::ContextState::capture() const
    noexcept(new_is_infallible_v) {
  auto* control = localControl();
  return control ? control->capture() : requestContext;
}

void RequestContext::ContextState::ensureShared() noexcept(
    new_is_infallible_v) {
  if (localControl() && !requestContext) {
    requestContext = capture();
  }
}

void RequestContext::ContextState::swap(ThreadLocalContext& other) noexcept {
  requestContext.swap(other.requestContext);
  std::swap(borrowedContext, other.borrowedContext);
}

RequestContext::ThreadLocalContext::ThreadLocalContext()
    : borrowedContext(nullptr) {}

RequestContextHandle::Control*
RequestContext::ThreadLocalContext::localControl() const noexcept {
  return borrowedContext.get();
}

RequestContext* RequestContext::ThreadLocalContext::get() const noexcept {
  auto* control = localControl();
  return control ? &control->context : requestContext.get();
}

std::shared_ptr<RequestContext> RequestContext::ThreadLocalContext::capture()
    const noexcept(new_is_infallible_v) {
  auto* control = localControl();
  return control ? control->capture() : requestContext;
}

void RequestContext::ThreadLocalContext::ensureShared() noexcept(
    new_is_infallible_v) {
  if (localControl() && !requestContext) {
    requestContext = capture();
  }
}

LocalRequestContextScopeGuard::LocalRequestContextScopeGuard()
    : owner_(makeDelayedDestructionUniquePtr<RequestContextHandle::Control>()),
      prev_(
          RequestContext::setContextState(
              RequestContext::ContextState(*owner_))) {}

LocalRequestContextScopeGuard::LocalRequestContextScopeGuard(intptr_t rootId)
    : owner_(
          makeDelayedDestructionUniquePtr<RequestContextHandle::Control>(
              rootId)),
      prev_(
          RequestContext::setContextState(
              RequestContext::ContextState(*owner_))) {}

LocalRequestContextScopeGuard::~LocalRequestContextScopeGuard() {
  owner_->checkThread();
  RequestContext::restoreContextState(std::move(prev_));
}

BorrowedRequestContextScopeGuard::BorrowedRequestContextScopeGuard(
    const RequestContextHandle& handle)
    : prev_(
          RequestContext::setContextState(
              RequestContext::ContextState(handle))) {}

BorrowedRequestContextScopeGuard::~BorrowedRequestContextScopeGuard() {
#ifndef NDEBUG
  DCHECK(thread_ == std::this_thread::get_id());
#endif
  RequestContext::restoreContextState(std::move(prev_));
}

RequestContext::RequestContext() : rootId_(reinterpret_cast<intptr_t>(this)) {}

RequestContext::RequestContext(intptr_t rootid) : rootId_(rootid) {}

RequestContext::RequestContext(const RequestContext& ctx, intptr_t rootid, Tag)
    : RequestContext(ctx) {
  rootId_ = rootid;
}

RequestContext::RequestContext(const RequestContext& ctx, Tag)
    : RequestContext(ctx) {}

/* static */ std::shared_ptr<RequestContext> RequestContext::copyAsRoot(
    const RequestContext& ctx, intptr_t rootid) {
  return std::make_shared<RequestContext>(ctx, rootid, Tag{});
}

/* static */ std::shared_ptr<RequestContext> RequestContext::copyAsChild(
    const RequestContext& ctx) {
  return std::make_shared<RequestContext>(ctx, Tag{});
}

void RequestContext::setContextData(
    const RequestToken& token, std::unique_ptr<RequestData> data) {
  state_.doSetContextData(token, data, DoSetBehaviour::SET, false);
}

bool RequestContext::setContextDataIfAbsent(
    const RequestToken& token, std::unique_ptr<RequestData> data) {
  return state_.doSetContextData(
      token, data, DoSetBehaviour::SET_IF_ABSENT, false);
}

void RequestContext::overwriteContextData(
    const RequestToken& token, std::unique_ptr<RequestData> data, bool safe) {
  state_.doSetContextData(token, data, DoSetBehaviour::OVERWRITE, safe);
}

bool RequestContext::hasContextData(const RequestToken& val) const {
  return state_.hasContextData(val);
}

RequestData* FOLLY_NULLABLE
RequestContext::getContextData(const RequestToken& val) {
  return state_.getContextData(val);
}

const RequestData* FOLLY_NULLABLE
RequestContext::getContextData(const RequestToken& val) const {
  return state_.getContextData(val);
}

void RequestContext::onSet() {
  state_.onSet();
}

void RequestContext::onUnset() {
  state_.onUnset();
}

void RequestContext::clearContextData(const RequestToken& val) {
  state_.clearContextData(val);
}

/* static */ std::shared_ptr<RequestContext> RequestContext::setContext(
    std::shared_ptr<RequestContext> const& newCtx) {
  return setContext(copy(newCtx));
}

/* static */ std::shared_ptr<RequestContext> RequestContext::setContext(
    std::shared_ptr<RequestContext>&& newCtx_) {
  auto newCtx = std::move(newCtx_); // enforce that it is really moved-from

  auto& staticCtx = getStaticContext();
  if (FOLLY_UNLIKELY(staticCtx.localControl() != nullptr)) {
    return setContextFromLocal(std::move(newCtx));
  }
  if (newCtx == staticCtx.requestContext) {
    return newCtx;
  }

  FOLLY_SDT(
      folly,
      request_context_switch_before,
      staticCtx.requestContext.get(),
      newCtx.get(),
      staticCtx.requestContext ? staticCtx.requestContext->getRootId() : 0,
      newCtx ? newCtx->getRootId() : 0);

  std::shared_ptr<RequestContext> prevCtx;
  RequestContext* curCtx = staticCtx.requestContext.get();
  bool checkCur = curCtx && curCtx->state_.combined();
  bool checkNew = newCtx && newCtx->state_.combined();
  if (checkCur && checkNew) {
    hazptr_array<2> h = make_hazard_pointer_array<2>();
    auto curc = h[0].protect(curCtx->state_.combined_);
    auto newc = h[1].protect(newCtx->state_.combined_);
    auto& curcb = curc->callbackData_;
    auto& newcb = newc->callbackData_;
    for (auto it = curcb.begin(); it != curcb.end(); ++it) {
      DCHECK(it.key());
      auto data = it.key();
      if (!newcb.contains(data)) {
        data->onUnset();
      }
    }
    prevCtx = std::move(staticCtx.requestContext);
    staticCtx.requestContext = std::move(newCtx);
    staticCtx.rootId.store(
        staticCtx.requestContext->getRootId(), std::memory_order_relaxed);
    for (auto it = newcb.begin(); it != newcb.end(); ++it) {
      DCHECK(it.key());
      auto data = it.key();
      if (!curcb.contains(data)) {
        data->onSet();
      }
    }
  } else {
    if (curCtx) {
      curCtx->state_.onUnset();
    }
    prevCtx = std::move(staticCtx.requestContext);
    staticCtx.requestContext = std::move(newCtx);
    if (staticCtx.requestContext) {
      staticCtx.rootId.store(
          staticCtx.requestContext->rootId_, std::memory_order_relaxed);
      staticCtx.requestContext->state_.onSet();
    } else {
      staticCtx.rootId.store(0, std::memory_order_relaxed);
    }
  }
  gAsyncSignalSafeRequestContextCache = staticCtx.requestContext.get();
  // Notify the Watchers via the registry
  getWatcherRegistry().invokeWatchers(prevCtx, staticCtx.requestContext);
  return prevCtx;
}

FOLLY_NOINLINE std::shared_ptr<RequestContext>
RequestContext::setContextFromLocal(std::shared_ptr<RequestContext>&& context) {
  ContextState state(std::move(context));
  getStaticContext().ensureShared();
  return std::move(setContextState(std::move(state)).requestContext);
}

/* static */ RequestContext::ContextState RequestContext::setContextState(
    ContextState&& newCtx) {
  auto& staticCtx = getStaticContext();
  if (!newCtx.localControl() && !staticCtx.localControl()) {
    return ContextState(setContext(std::move(newCtx.requestContext)));
  }
  return setContextImpl(std::move(newCtx), staticCtx, staticCtx.rootId, true);
}

/* static */ void RequestContext::restoreContextState(ContextState&& context) {
  auto& staticCtx = getStaticContext();
  if (!context.localControl() && !staticCtx.localControl()) {
    setContext(std::move(context.requestContext));
  } else {
    setContextImpl(std::move(context), staticCtx, staticCtx.rootId, true);
  }
}

RequestContext::ContextState RequestContext::setContextImpl(
    ContextState&& newCtx,
    ThreadLocalContext& current,
    std::atomic<intptr_t>& rootId,
    bool notifyWatchers) {
  auto* newContext = newCtx.get();
  auto* currentContext = current.get();
  if (newContext == currentContext) {
    if (newCtx.localControl() && !current.localControl()) {
      return ContextState(current.requestContext);
    }
    return std::move(newCtx);
  }

  if (notifyWatchers &&
      getWatcherRegistry().watchers_.load(std::memory_order_acquire)) {
    newCtx.ensureShared();
    current.ensureShared();
  }

  FOLLY_SDT(
      folly,
      request_context_switch_before,
      currentContext,
      newContext,
      currentContext ? currentContext->getRootId() : 0,
      newContext ? newContext->getRootId() : 0);

  ContextState prevCtx;
  RequestContext* curCtx = currentContext;
  bool checkCur = curCtx && curCtx->state_.combined();
  bool checkNew = newContext && newContext->state_.combined();
  if (checkCur && checkNew) {
    hazptr_array<2> h = make_hazard_pointer_array<2>();
    auto curc = h[0].protect(curCtx->state_.combined_);
    auto newc = h[1].protect(newContext->state_.combined_);
    auto& curcb = curc->callbackData_;
    auto& newcb = newc->callbackData_;
    for (auto it = curcb.begin(); it != curcb.end(); ++it) {
      DCHECK(it.key());
      auto data = it.key();
      if (!newcb.contains(data)) {
        data->onUnset();
      }
    }
    prevCtx.swap(current);
    newCtx.swap(current);
    rootId.store(current.get()->getRootId(), std::memory_order_relaxed);
    for (auto it = newcb.begin(); it != newcb.end(); ++it) {
      DCHECK(it.key());
      auto data = it.key();
      if (!curcb.contains(data)) {
        data->onSet();
      }
    }
  } else {
    if (curCtx) {
      curCtx->state_.onUnset();
    }
    prevCtx.swap(current);
    newCtx.swap(current);
    if (auto* activeContext = current.get()) {
      rootId.store(activeContext->rootId_, std::memory_order_relaxed);
      activeContext->state_.onSet();
    } else {
      rootId.store(0, std::memory_order_relaxed);
    }
  }
  gAsyncSignalSafeRequestContextCache = current.get();
  if (notifyWatchers) {
    auto* watchers =
        getWatcherRegistry().watchers_.load(std::memory_order_acquire);
    if (watchers) {
      auto rollback = makeGuard([&]() noexcept {
        restoreContextAfterPromotionFailure(
            std::move(prevCtx), current, rootId);
      });
      prevCtx.ensureShared();
      current.ensureShared();
      rollback.dismiss();
    }
    // Notify the Watchers via the registry
    getWatcherRegistry().invokeWatchers(
        watchers, prevCtx.requestContext, current.requestContext);
  }
  return prevCtx;
}

FOLLY_NOINLINE void RequestContext::restoreContextAfterPromotionFailure(
    ContextState&& context,
    ThreadLocalContext& current,
    std::atomic<intptr_t>& rootId) {
  setContextImpl(std::move(context), current, rootId, false);
}

/* static */ RequestContext::SetContextWatcherRegistry&
RequestContext::getWatcherRegistry() {
  static SetContextWatcherRegistry registry;

  return registry;
}

/* static */ void RequestContext::addSetContextWatcher(
    RequestContext::SetContextWatcherSig& func) {
  getWatcherRegistry().addWatcher(func);
}

/* static */ std::shared_ptr<RequestContext> RequestContext::saveContext() {
  auto* staticContext = tryGetStaticContext();
  if (FOLLY_UNLIKELY(staticContext && staticContext->localControl())) {
    return saveContextFromLocal(*staticContext);
  }
  return staticContext ? staticContext->requestContext : nullptr;
}

FOLLY_NOINLINE std::shared_ptr<RequestContext>
RequestContext::saveContextFromLocal(ThreadLocalContext& context) noexcept(
    new_is_infallible_v) {
  return context.capture();
}

/* static */ RequestContextHandle RequestContext::saveContextLocal() {
  RequestContextHandle handle;
  auto* state = tryGetStaticContext();
  if (state == nullptr) {
    return handle;
  }
  if (auto* control = state->localControl()) {
    handle.control_ = control->pin();
  } else if (state->requestContext) {
    throw std::logic_error("saveContextLocal requires a local context");
  }
  return handle;
}

RequestContext::StaticContext::~StaticContext() {
  // If there is an active request context, reset requestContext before
  // destroying it, as RequestData destructors (or onClear()) could try to
  // access the current request context and copy a shared_ptr while being
  // destroyed.
  std::ignore = std::exchange(requestContext, {});
}

RequestContext::ThreadLocalContext::~ThreadLocalContext() {
  if (borrowedContext) {
    gAsyncSignalSafeRequestContextCache = nullptr;
    ContextState context;
    context.swap(*this);
  }
}

/* static */ RequestContext::ThreadLocalContext&
RequestContext::getStaticContext() {
  return StaticContextThreadLocal::get();
}

/* static */ RequestContext::ThreadLocalContext*
RequestContext::tryGetStaticContext() {
  return StaticContextThreadLocal::try_get();
}

/* static */ RequestContext::StaticContextAccessor
RequestContext::accessAllThreads() {
  return StaticContextAccessor{StaticContextThreadLocal::accessAllThreads()};
}

/* static */ std::vector<RequestContext::RootIdInfo>
RequestContext::getRootIdsFromAllThreads() {
  std::vector<RootIdInfo> result;
  auto accessor = RequestContext::accessAllThreads();
  for (auto it = accessor.begin(); it != accessor.end(); ++it) {
    result.push_back(it.getRootIdInfo());
  }
  return result;
}

/* static */ std::shared_ptr<RequestContext>
RequestContext::setShallowCopyContext() {
  if (getStaticContext().localControl()) {
    getStaticContext().ensureShared();
    return std::move(setShallowCopyContextState().requestContext);
  }
  auto& parent = getStaticContext().requestContext;
  auto child = parent
      ? RequestContext::copyAsChild(*parent)
      : std::make_shared<RequestContext>();
  if (!parent) {
    child->rootId_ = 0;
  }
  // Do not use setContext to avoid global set/unset
  // Also rootId does not change so do not bother setting it.
  std::swap(child, parent);
  gAsyncSignalSafeRequestContextCache = parent.get();
  getWatcherRegistry().invokeWatchers(child, parent);
  return child;
}

/* static */ RequestContext::ContextState
RequestContext::setShallowCopyContextState() {
  auto& staticCtx = getStaticContext();
  auto* parent = staticCtx.get();
  auto child = parent
      ? RequestContext::copyAsChild(*parent)
      : std::make_shared<RequestContext>();
  if (!parent) {
    child->rootId_ = 0;
  }
  // Do not use setContext to avoid global set/unset
  // Also rootId does not change so do not bother setting it.
  auto* watchers =
      getWatcherRegistry().watchers_.load(std::memory_order_acquire);
  if (watchers) {
    staticCtx.ensureShared();
  }
  ContextState previous;
  previous.swap(staticCtx);
  ContextState current(std::move(child));
  current.swap(staticCtx);
  gAsyncSignalSafeRequestContextCache = staticCtx.get();
  getWatcherRegistry().invokeWatchers(
      watchers, previous.requestContext, staticCtx.requestContext);
  return previous;
}

/* static */ RequestContext* RequestContext::get() {
  if (auto* staticContext = tryGetStaticContext()) {
    if (auto* context = staticContext->get()) {
      return context;
    }
  }

  static RequestContext defaultContext(0);
  return std::addressof(defaultContext);
}

/* static */ RequestContext* RequestContext::try_get() {
  if (auto* staticContext = tryGetStaticContext()) {
    return staticContext->get();
  }
  return nullptr;
}

void RequestContextSaverScopeGuard::setContext(
    std::shared_ptr<RequestContext>&& ctx) {
  // Consume the argument even when there is nothing to replace, like
  // RequestContext::setContext() does.
  auto newCtx = std::move(ctx);
  if (RequestContext::try_get() == newCtx.get()) {
    return; // Nothing to replace.
  }
  // Destruction of the current context happens under prev_ context.
  std::ignore = RequestContext::setContext(std::move(prev_));
  prev_ = RequestContext::setContext(std::move(newCtx));
}

void RequestContextSaverScopeGuard::restoreContext() {
  if (RequestContext::try_get() == prev_.get()) {
    return; // Avoid a shared_ptr copy if there is nothing to do.
  }
  std::ignore = RequestContext::setContext(prev_);
}

#ifndef NDEBUG
DCheckRequestContextRestoredGuard::~DCheckRequestContextRestoredGuard() {
  CHECK_EQ(prev_.get(), RequestContext::try_get());
}
#endif

} // namespace folly
