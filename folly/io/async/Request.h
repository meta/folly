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

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <folly/SharedMutex.h>
#include <folly/SingletonThreadLocal.h>
#include <folly/Synchronized.h>
#include <folly/concurrency/ProcessLocalUniqueId.h>
#include <folly/container/F14Map.h>
#include <folly/detail/Iterators.h>
#include <folly/io/async/DelayedDestruction.h>
#include <folly/lang/New.h>
#include <folly/synchronization/Hazptr.h>

namespace folly {

/*
 * A token to be used to fetch data from RequestContext.
 * Generally you will want this to be a static, created only once using a
 * string, and then only copied. The string constructor is expensive.
 */
class RequestToken {
 public:
  RequestToken() = default;
  explicit RequestToken(const std::string& str);

  bool operator==(const RequestToken& other) const {
    return token_ == other.token_;
  }

  // Slow, use only for debug log messages.
  std::string getDebugString() const;

  friend struct std::hash<folly::RequestToken>;

 private:
  static Synchronized<F14FastMap<std::string, uint32_t>>& getCache();

  uint32_t token_;
};
static_assert(
    std::is_trivially_destructible<RequestToken>::value,
    "must be trivially destructible");

} // namespace folly

namespace std {
template <>
struct hash<folly::RequestToken> {
  size_t operator()(const folly::RequestToken& token) const {
    return hash<uint32_t>()(token.token_);
  }
};
} // namespace std

namespace folly {

// Some request context that follows an async request through a process
// Everything in the context must be thread safe

class RequestData {
 public:
  virtual ~RequestData() = default;

  // Avoid calling RequestContext::setContextData, setContextDataIfAbsent, or
  // clearContextData from these callbacks. Doing so will cause deadlock. We
  // could fix these deadlocks, but only at significant performance penalty, so
  // just don't do it!

  // hasCallback() applies only to onSet() and onUnset().
  // onClear() is always executed exactly once.
  virtual bool hasCallback() = 0;
  // Callback executed when setting RequestContext. Make sure your RequestData
  // instance overrides the hasCallback method to return true otherwise
  // the callback will not be executed
  virtual void onSet() {}
  // Callback executed when unsetting RequestContext. Make sure your RequestData
  // instance overrides the hasCallback method to return true otherwise
  // the callback will not be executed
  virtual void onUnset() {}
  // Callback executed exactly once upon the release of the last
  // reference to the request data (as a result of either a call to
  // clearContextData or the destruction of a request context that
  // contains a reference to the data). It can be overridden in
  // derived classes. There may be concurrent executions of onSet()
  // and onUnset() with that of onClear().
  virtual void onClear() {}
  // If false, the data cannot be removed from a context that may be shared:
  // clearContextData, a duplicate setContextData, and overwriteContextData with
  // safe = false throw std::invalid_argument. It can still be replaced or
  // removed by the ShallowCopyRequestContextScopeGuard constructors.
  //
  // clearContextData() is a flawed primitive, since there is no safe
  // way of accessing a pointer returned by getContextData() if there
  // is a chance that the data is cleared concurrently. Support is
  // only kept for legacy reasons; new RequestData implementations
  // should set this to false, and instead use the shallow-copy
  // pattern to remove or replace the object.
  virtual bool clearable() { return true; }
  // For debugging
  int refCount() { return keepAliveCounter_.load(std::memory_order_acquire); }

 private:
  // For efficiency, RequestContext provides a raw ptr interface.
  // To support shallow copy, we need a shared ptr.
  // To keep it as safe as possible (even if a raw ptr is passed back),
  // the counter lives directly in RequestData.

  friend class RequestContext;

  static constexpr int kDeleteCount = 0x1;
  static constexpr int kClearCount = 0x1000;
  static constexpr int kClearDeleteCounts = kClearCount + kDeleteCount;

  // Reference-counting functions.
  // Increment the reference count.
  void acquireRef();
  // Decrement the reference count. Clear only if last.
  void releaseRefClearOnly();
  // Decrement the reference count. Delete only if last.
  void releaseRefDeleteOnly();
  // Decrement the reference count. Clear and delete if last.
  void releaseRefClearDelete();
  void releaseRefClearDeleteSlow();

  std::atomic<int> keepAliveCounter_{0};
};

/**
 * ImmutableRequestData is a folly::RequestData that holds an immutable value.
 * It is thread-safe (a requirement of RequestData) because it is immutable.
 */
template <typename T>
class ImmutableRequestData : public folly::RequestData {
 public:
  template <
      typename... Args,
      typename = typename std::enable_if<
          std::is_constructible<T, Args...>::value>::type>
  explicit ImmutableRequestData(Args&&... args) noexcept(
      std::is_nothrow_constructible<T, Args...>::value)
      : val_(std::forward<Args>(args)...) {}

  const T& value() const { return val_; }

  bool hasCallback() override { return false; }

 private:
  const T val_;
};

using RequestDataItem = std::pair<RequestToken, std::unique_ptr<RequestData>>;

class RequestContext;
class BorrowedRequestContextScopeGuard;
class LocalRequestContextScopeGuard;

// Thread-confined lifetime pin for an ambient-owned context. Pin access,
// borrowed activation, and release stay on the creating thread. Capture shared
// ownership before offload; CPU work uses the captured shared context instead.
class RequestContextHandle {
 public:
  RequestContextHandle() noexcept;
  ~RequestContextHandle();

  RequestContextHandle(const RequestContextHandle&) = delete;
  RequestContextHandle& operator=(const RequestContextHandle&) = delete;
  RequestContextHandle(RequestContextHandle&& other) noexcept;
  RequestContextHandle& operator=(RequestContextHandle&& other) noexcept;

  RequestContext* get() const noexcept;
  RequestContext* operator->() const noexcept { return get(); }
  explicit operator bool() const noexcept { return get() != nullptr; }
  void reset() noexcept;

  // Called on the creating thread before offload. Acquires a shared lifetime
  // without changing identity; subsequent captures reuse that ownership.
  std::shared_ptr<RequestContext> capture() const noexcept(new_is_infallible_v);

 private:
  friend class RequestContext;
  friend class LocalRequestContextScopeGuard;
  struct Control;
  DelayedDestructionBase::IntrusivePtr<Control> control_;
};

class RequestContext {
 public:
  RequestContext();
  RequestContext(RequestContext&& ctx) = delete;
  RequestContext& operator=(const RequestContext&) = delete;
  RequestContext& operator=(RequestContext&&) = delete;

  // copy ctor is disabled, use copyAsRoot/copyAsChild instead.
  static std::shared_ptr<RequestContext> copyAsRoot(
      const RequestContext& ctx, intptr_t rootid);
  static std::shared_ptr<RequestContext> copyAsChild(const RequestContext& ctx);

  // Creates a unique request context for this request, and sets it as current
  // context. It will be propagated between queues / threads (where
  // implemented).
  //
  // Whenever possible, prefer RequestContextScopeGuard instead of create() to
  // make sure that RequestContext is reset to the original value when we exit
  // the scope.
  //
  // Returns the previous request context.
  static std::shared_ptr<RequestContext> create() {
    return setContext(std::make_shared<RequestContext>());
  }

  // Returns the current context, if set, otherwise returns the default global
  // request context.
  //
  // NOTE: This is a legacy method: there is almost never a good reason to use
  // the default global request context. Prefer try_get() for new code.
  static RequestContext* get();

  // Get the current context, if it has already been set, or nullptr.
  static RequestContext* try_get();

  intptr_t getRootId() const { return rootId_; }

  // Returns a non-zero, process-unique identifier for this context's stored
  // RequestData objects. Copies receive a distinct version. Set, overwrite,
  // and clear operations publish a new version after their changes are visible;
  // a reader that observes the version also observes those changes. The version
  // does not change when a stored RequestData mutates its own state, and may
  // change when an operation makes no change. Only equality within this process
  // is meaningful; do not persist, order, or compare versions across processes.
  //
  // To cache a derived value, read the version before computing the value and
  // associate the result with that snapshot. This method is thread-safe.
  uint64_t getContextDataVersion() const noexcept;

  struct RootIdInfo {
    intptr_t id;
    std::thread::id tid;
    uint64_t tidOS;
  };
  static std::vector<RootIdInfo> getRootIdsFromAllThreads();

  // The following APIs are used to add, remove and access RequestData instance
  // in the RequestContext instance, normally used for per-RequestContext
  // tracking or callback on set and unset. These APIs are Thread-safe.
  // These APIs are performance sensitive, so please ask if you need help
  // profiling any use of these APIs.

  // Add RequestData instance "data" to this RequestContext instance, with
  // string identifier "val". If the same string identifier has already been
  // used, will print a warning message for the first time, clear the existing
  // RequestData instance for "val", and **not** add "data".
  void setContextData(
      const RequestToken& token, std::unique_ptr<RequestData> data);
  void setContextData(
      const std::string& val, std::unique_ptr<RequestData> data) {
    setContextData(RequestToken(val), std::move(data));
  }

  // Add RequestData instance "data" to this RequestContext instance, with
  // string identifier "val". If the same string identifier has already been
  // used, return false and do nothing. Otherwise add "data" and return true.
  bool setContextDataIfAbsent(
      const RequestToken& token, std::unique_ptr<RequestData> data);
  bool setContextDataIfAbsent(
      const std::string& val, std::unique_ptr<RequestData> data) {
    return setContextDataIfAbsent(RequestToken(val), std::move(data));
  }

  // Remove the RequestData instance with string identifier "val", if it exists.
  void clearContextData(const RequestToken& val);
  void clearContextData(const std::string& val) {
    clearContextData(RequestToken(val));
  }

  // Returns true if and only if the RequestData instance with string identifier
  // "val" exists in this RequestContext instnace.
  bool hasContextData(const RequestToken& val) const;
  bool hasContextData(const std::string& val) const {
    return hasContextData(RequestToken(val));
  }

  // Get (constant) raw pointer of the RequestData instance with string
  // identifier "val" if it exists, otherwise returns null pointer.
  RequestData* getContextData(const RequestToken& val);
  const RequestData* getContextData(const RequestToken& val) const;
  RequestData* getContextData(const std::string& val) {
    return getContextData(RequestToken(val));
  }
  const RequestData* getContextData(const std::string& val) const {
    return getContextData(RequestToken(val));
  }

  // Same as getContextData(), but caching the RequestData pointer in
  // thread-local storage to avoid the lookup cost. The thread cache is
  // invalidated if the current request context changes or it gets modified.
  //
  // This can be used for RequestData that are queried very frequently. It
  // should almost always be faster than getContextData(), but it consumes
  // thread-local storage space, so it is worth doing only when a high hit rate
  // is expected.
  //
  // The storage for the caches is associated to a type tag passed as template
  // argument. This tag also contains the RequestToken key as a static member
  // kToken. This guarantees that a given tag cannot be accidentally used with
  // multiple tokens.
  //
  // For example:
  //
  // struct MyRequestDataTraits {
  //   static inline const RequestToken kToken{"my_request_data"};
  // };
  //
  // ...
  // auto* data = ctx->getThreadCachedContextData<MyRequestDataTraits>();
  //
  template <class Traits>
  RequestData* getThreadCachedContextData();

  void onSet();
  void onUnset();

  // The following API is used to pass the context through queues / threads.
  // saveContext() is called to get a shared_ptr to the context, and
  // setContext() is used to reset it on the other side of the queue.
  //
  // WARNING: setContext() should never be used directly, as it is error-prone:
  // the caller must save the previous context and restore it on every exit
  // path, and any mistake (a missed restore, an exception thrown in between)
  // leaks a foreign context onto the thread, which then silently propagates
  // to unrelated work running there. Use RequestContextScopeGuard or
  // RequestContextSaverScopeGuard instead, which restore the original context
  // automatically when they go out of scope.
  //
  // A shared_ptr is used, because many request may fan out across
  // multiple threads, or do post-send processing, etc.
  //
  // Returns the previous request context.
  static std::shared_ptr<RequestContext> setContext(
      std::shared_ptr<RequestContext> const& ctx);
  static std::shared_ptr<RequestContext> setContext(
      std::shared_ptr<RequestContext>&& newCtx_);

  // Watchers receive owning pointers to the previous and current contexts
  // after a context switch.
  using SetContextWatcherSig = void(
      const std::shared_ptr<RequestContext>& prev,
      const std::shared_ptr<RequestContext>& ctx);
  static void addSetContextWatcher(SetContextWatcherSig& func);

 private:
  struct SetContextWatcherRegistry {
    using Sig = SetContextWatcherSig;

    struct Watcher {
      Sig* func_;
      Watcher* next_{nullptr};

      explicit Watcher(Sig& func) : func_(&func) {}
    };

    std::atomic<Watcher*> watchers_{nullptr};

    void addWatcher(Sig& func) {
      auto watcherPtr = new Watcher(func);
      auto* head = watchers_.load(std::memory_order_relaxed);
      do {
        watcherPtr->next_ = head;
      } while (!watchers_.compare_exchange_weak(
          head,
          watcherPtr,
          std::memory_order_acq_rel,
          std::memory_order_relaxed));
    }

    void invokeWatchers(
        const std::shared_ptr<RequestContext>& prev,
        const std::shared_ptr<RequestContext>& ctx) {
      invokeWatchers(watchers_.load(std::memory_order_acquire), prev, ctx);
    }

    void invokeWatchers(
        Watcher* watcher,
        const std::shared_ptr<RequestContext>& prev,
        const std::shared_ptr<RequestContext>& ctx) {
      while (watcher != nullptr) {
        watcher->func_(prev, ctx);
        watcher = watcher->next_;
      }
    }

    ~SetContextWatcherRegistry() {
      auto* watcher = watchers_.exchange(nullptr, std::memory_order_acquire);
      while (watcher != nullptr) {
        delete std::exchange(watcher, watcher->next_);
      }
    }
  };

  static SetContextWatcherRegistry& getWatcherRegistry();

 public:
  static std::shared_ptr<RequestContext> saveContext();

  // Pins a local ambient context without requesting shared ownership. The
  // binding must be local or empty; use saveContext() for shared bindings.
  static RequestContextHandle saveContextLocal();

 private:
  struct Tag {};
  RequestContext(const RequestContext& ctx) = default;

 public:
  RequestContext(const RequestContext& ctx, intptr_t rootid, Tag tag);
  RequestContext(const RequestContext& ctx, Tag tag);
  explicit RequestContext(intptr_t rootId);

 private:
  friend class BorrowedRequestContextScopeGuard;
  friend class LocalRequestContextScopeGuard;
  struct ThreadLocalContext;

  struct ContextState {
    ContextState();
    explicit ContextState(std::shared_ptr<RequestContext> context);
    explicit ContextState(RequestContextHandle::Control& control);
    explicit ContextState(const RequestContextHandle& handle);
    ~ContextState();
    ContextState(const ContextState&) = delete;
    ContextState& operator=(const ContextState&) = delete;
    ContextState(ContextState&& other) noexcept;
    ContextState& operator=(ContextState&& other) noexcept;
    void swap(ContextState& other) noexcept;
    void swap(ThreadLocalContext& other) noexcept;

    RequestContext* get() const noexcept;
    RequestContextHandle::Control* localControl() const noexcept;
    std::shared_ptr<RequestContext> capture() const
        noexcept(new_is_infallible_v);
    void ensureShared() noexcept(new_is_infallible_v);

    std::shared_ptr<RequestContext> requestContext;
    DelayedDestructionBase::IntrusivePtr<RequestContextHandle::Control>
        borrowedContext;
  };

  static ContextState setContextState(ContextState&& context);
  static void restoreContextState(ContextState&& context);
  static std::shared_ptr<RequestContext> setContextFromLocal(
      std::shared_ptr<RequestContext>&& context);
  static std::shared_ptr<RequestContext> saveContextFromLocal(
      ThreadLocalContext& context) noexcept(new_is_infallible_v);
  static ContextState setContextImpl(
      ContextState&& context,
      ThreadLocalContext& current,
      std::atomic<intptr_t>& rootId,
      bool notifyWatchers);
  static void restoreContextAfterPromotionFailure(
      ContextState&& context,
      ThreadLocalContext& current,
      std::atomic<intptr_t>& rootId);
  static ContextState setShallowCopyContextState();

 public:
  struct StaticContext {
    StaticContext() = default;
    ~StaticContext();

    StaticContext(const StaticContext&) = delete;
    StaticContext(StaticContext&&) = delete;
    StaticContext& operator=(const StaticContext&) = delete;
    StaticContext& operator=(StaticContext&&) = delete;

    std::shared_ptr<RequestContext> requestContext;
    std::atomic<intptr_t> rootId{0};
  };

 private:
  struct ThreadLocalContext : StaticContext {
    ThreadLocalContext();
    ~ThreadLocalContext();

    ThreadLocalContext(const ThreadLocalContext&) = delete;
    ThreadLocalContext(ThreadLocalContext&&) = delete;
    ThreadLocalContext& operator=(const ThreadLocalContext&) = delete;
    ThreadLocalContext& operator=(ThreadLocalContext&&) = delete;

    RequestContextHandle::Control* localControl() const noexcept;
    RequestContext* get() const noexcept;
    std::shared_ptr<RequestContext> capture() const
        noexcept(new_is_infallible_v);
    void ensureShared() noexcept(new_is_infallible_v);

    DelayedDestructionBase::IntrusivePtr<RequestContextHandle::Control>
        borrowedContext;
  };

  static ThreadLocalContext& getStaticContext();
  static ThreadLocalContext* tryGetStaticContext();

  using StaticContextThreadLocal = SingletonThreadLocal<
      RequestContext::ThreadLocalContext,
      RequestContext /* Tag */>;

 public:
  // The holder lives until thread exit. Local bindings may have an empty
  // requestContext field; use accessAllThreads() to inspect their context.
  static StaticContext& getThreadContextHolder();

  class StaticContextAccessor {
   private:
    using Inner = StaticContextThreadLocal::Accessor;
    using IteratorBase = Inner::Iterator;
    using IteratorTag = typename IteratorBase::iterator_category;

    Inner inner_;

    explicit StaticContextAccessor(Inner&& inner) noexcept
        : inner_(std::move(inner)) {}

   public:
    friend class RequestContext;

    class Iterator
        : public detail::IteratorAdaptor<
              Iterator,
              IteratorBase,
              StaticContext,
              IteratorTag> {
      using Super = detail::
          IteratorAdaptor<Iterator, IteratorBase, StaticContext, IteratorTag>;

     public:
      using Super::Super;

      StaticContext& dereference() const { return *base(); }

      // Does not acquire ownership. Coordinate with the owning thread to keep
      // its binding unchanged and context alive while inspecting the result.
      RequestContext* getContext() const noexcept { return base()->get(); }

      RootIdInfo getRootIdInfo() const {
        RootIdInfo info;
        info.id = base()->rootId.load(std::memory_order_relaxed);
        info.tid = base().getThreadId();
        info.tidOS = base().getOSThreadId();
        return info;
      }
    };

    StaticContextAccessor(const StaticContextAccessor&) = delete;
    StaticContextAccessor& operator=(const StaticContextAccessor&) = delete;
    StaticContextAccessor(StaticContextAccessor&&) = default;
    StaticContextAccessor& operator=(StaticContextAccessor&&) = default;

    Iterator begin() const { return Iterator(inner_.begin()); }
    Iterator end() const { return Iterator(inner_.end()); }
  };
  // Returns an accessor object that blocks the construction and destruction of
  // StaticContext objects on all threads. This is useful to quickly introspect
  // the context from all threads while ensuring that their thread-local
  // StaticContext object is not destroyed. Local bindings may have an empty
  // requestContext field; Iterator::getContext() also exposes local contexts.
  // The accessor does not prevent context switches on the owning threads.
  static StaticContextAccessor accessAllThreads();

  // Start shallow copy guard implementation details:
  // All methods are private to encourage proper use
  friend struct ShallowCopyRequestContextScopeGuard;

  // This sets a shallow copy of the current context as current, then return the
  // previous context (so it can be reset later).
  //
  // WARNING: setShallowCopyContext() should never be used directly, as it is
  // error-prone for the same reasons as setContext() (see above). Use
  // ShallowCopyRequestContextScopeGuard instead, which restores the original
  // context automatically when it goes out of scope.
  static std::shared_ptr<RequestContext> setShallowCopyContext();

  // For functions with a parameter safe, if safe is true then the
  // caller guarantees that there are no concurrent readers or writers
  // accessing the structure.
  void overwriteContextData(
      const RequestToken& token,
      std::unique_ptr<RequestData> data,
      bool safe = false);
  void overwriteContextData(
      const std::string& val,
      std::unique_ptr<RequestData> data,
      bool safe = false) {
    overwriteContextData(RequestToken(val), std::move(data), safe);
  }

  enum class DoSetBehaviour {
    SET,
    SET_IF_ABSENT,
    OVERWRITE,
  };

  bool doSetContextDataHelper(
      const RequestToken& token,
      std::unique_ptr<RequestData>& data,
      DoSetBehaviour behaviour,
      bool safe = false);
  bool doSetContextDataHelper(
      const std::string& val,
      std::unique_ptr<RequestData>& data,
      DoSetBehaviour behaviour,
      bool safe = false) {
    return doSetContextDataHelper(RequestToken(val), data, behaviour, safe);
  }

  // State implementation with single-writer multi-reader data
  // structures protected by hazard pointers for readers and a lock
  // for writers.
  struct State {
    // Hazard pointer-protected combined structure for request data
    // and callbacks.
    struct Combined;
    hazptr_obj_cohort<> cohort_; // For destruction order
    std::atomic<Combined*> combined_{nullptr};
    // Version used to invalidate caches of values derived from context data. A
    // process-wide unique id is used (instead of, for example, a local counter)
    // so that it is not necessary to compare the request context pointer as
    // well. This saves one word of TLS and one comparison.
    std::atomic<uint64_t> version_{processLocalUniqueId()};
    // This should never be used directly. Locked mutations must use LockGuard;
    // caller-synchronized mutations must use VersionGuard.
    mutable folly::SharedMutex mutex_; // small exclusive mutex

    State();
    State(const State& o);
    State(State&&) = delete;
    State& operator=(const State&) = delete;
    State& operator=(State&&) = delete;
    ~State();

   private:
    friend class RequestContext;

    struct SetContextDataResult {
      bool changed; // Changes were made
      bool unexpected; // Update was unexpected
      Combined* replaced; // The combined structure was replaced
    };

    class VersionGuard;
    class LockGuard;

    Combined* combined() const;
    Combined* ensureCombined(); // Lazy allocation if needed
    void setCombined(Combined* p);
    Combined* expand(Combined* combined);
    bool doSetContextData(
        const RequestToken& token,
        std::unique_ptr<RequestData>& data,
        DoSetBehaviour behaviour,
        bool safe);
    bool hasContextData(const RequestToken& token) const;
    RequestData* getContextData(const RequestToken& token);
    const RequestData* getContextData(const RequestToken& token) const;
    void onSet();
    void onUnset();
    void clearContextData(const RequestToken& token);
    SetContextDataResult doSetContextDataHelper(
        const RequestToken& token,
        std::unique_ptr<RequestData>& data,
        DoSetBehaviour behaviour,
        bool safe);
    Combined* eraseOldData(
        Combined* cur,
        const RequestToken& token,
        RequestData* oldData,
        bool safe);
    Combined* insertNewData(
        Combined* cur,
        const RequestToken& token,
        std::unique_ptr<RequestData>& data,
        bool tokenWasPresent);
  }; // State
  State state_;
  // Shallow copies keep a note of the root context
  intptr_t rootId_;
};
static_assert(sizeof(RequestContext) <= 64, "unexpected size");

// Creates an ambient-owned context and restores the previous binding on exit.
// Leaving the scope requests destruction; local handles delay destruction
// without atomic ownership operations. This scope stays on its creating thread.
class LocalRequestContextScopeGuard {
 public:
  [[nodiscard]] LocalRequestContextScopeGuard();
  [[nodiscard]] explicit LocalRequestContextScopeGuard(intptr_t rootId);
  ~LocalRequestContextScopeGuard();

  LocalRequestContextScopeGuard(const LocalRequestContextScopeGuard&) = delete;
  LocalRequestContextScopeGuard& operator=(
      const LocalRequestContextScopeGuard&) = delete;
  LocalRequestContextScopeGuard(LocalRequestContextScopeGuard&&) = delete;
  LocalRequestContextScopeGuard& operator=(LocalRequestContextScopeGuard&&) =
      delete;

 private:
  DelayedDestructionUniquePtr<RequestContextHandle::Control> owner_;
  RequestContext::ContextState prev_;
};

// Activates a pinned context and restores the previous binding on exit.
// The caller may release its handle while this thread-confined scope is active.
class BorrowedRequestContextScopeGuard {
 public:
  [[nodiscard]] explicit BorrowedRequestContextScopeGuard(
      const RequestContextHandle& handle);
  ~BorrowedRequestContextScopeGuard();

  BorrowedRequestContextScopeGuard(const BorrowedRequestContextScopeGuard&) =
      delete;
  BorrowedRequestContextScopeGuard& operator=(
      const BorrowedRequestContextScopeGuard&) = delete;
  BorrowedRequestContextScopeGuard(BorrowedRequestContextScopeGuard&&) = delete;
  BorrowedRequestContextScopeGuard& operator=(
      BorrowedRequestContextScopeGuard&&) = delete;

 private:
#ifndef NDEBUG
  const std::thread::id thread_ = std::this_thread::get_id();
#endif
  RequestContext::ContextState prev_;
};

/**
 * RequestContextSaverScopeGuard allows to replace the current context
 * without switching back to original context, while ensuring that the original
 * context is restored on guard destruction.
 *
 * The constructor saves the current context but does not replace it; instead,
 * the setContext() method should be called to replace the current context. The
 * original context will be restored on guard destruction, and any time
 * restoreContext() is called. This is different from RequestContextScopeGuard
 * which replaces the current context in construction.
 *
 * This enables taking advantage of the optimization in setContext() which skips
 * invoking the RequestData callbacks if the new context is the the same as the
 * current one. The use case is processing tasks in a loop which are likely to
 * share the same context.
 */
class RequestContextSaverScopeGuard {
 public:
  [[nodiscard]] RequestContextSaverScopeGuard()
      : RequestContextSaverScopeGuard(RequestContext::saveContext()) {}

  ~RequestContextSaverScopeGuard() {
    RequestContext::setContext(std::move(prev_));
  }

  RequestContextSaverScopeGuard(const RequestContextSaverScopeGuard&) = delete;
  RequestContextSaverScopeGuard& operator=(
      const RequestContextSaverScopeGuard&) = delete;
  RequestContextSaverScopeGuard(RequestContextSaverScopeGuard&&) = delete;
  RequestContextSaverScopeGuard& operator=(RequestContextSaverScopeGuard&&) =
      delete;

  void setContext(std::shared_ptr<RequestContext>&& ctx);
  void restoreContext();

 protected:
  explicit RequestContextSaverScopeGuard(std::shared_ptr<RequestContext>&& ctx)
      : prev_(std::move(ctx)) {}

 private:
  std::shared_ptr<RequestContext> prev_;
};

/**
 * Note: you probably want to use ShallowCopyRequestContextScopeGuard
 * This resets all other RequestData for the duration of the scope!
 */
class RequestContextScopeGuard : private RequestContextSaverScopeGuard {
 public:
  // Create a new RequestContext and reset to the original value when
  // this goes out of scope.
  [[nodiscard]] RequestContextScopeGuard()
      : RequestContextSaverScopeGuard(RequestContext::create()) {}

  // Set a RequestContext that was previously captured by saveContext(). It will
  // be automatically reset to the original value when this goes out of scope.
  [[nodiscard]] explicit RequestContextScopeGuard(
      std::shared_ptr<RequestContext> const& ctx)
      : RequestContextSaverScopeGuard(RequestContext::setContext(ctx)) {}
  [[nodiscard]] explicit RequestContextScopeGuard(
      std::shared_ptr<RequestContext>&& ctx)
      : RequestContextSaverScopeGuard(
            RequestContext::setContext(std::move(ctx))) {}
};

/**
 * This guard maintains all the RequestData pointers of the parent.
 * This allows to overwrite a specific RequestData pointer for the
 * scope's duration, without breaking others.
 *
 * Only modified pointers will have their set/onset methods called
 */
struct ShallowCopyRequestContextScopeGuard {
  [[nodiscard]] ShallowCopyRequestContextScopeGuard()
      : prev_(RequestContext::setShallowCopyContext()) {}

  /**
   * Shallow copy then overwrite one specific RequestData
   *
   * Helper constructor which is a more efficient equivalent to
   * "clearRequestData" then "setRequestData" after the guard.
   */
  [[nodiscard]] ShallowCopyRequestContextScopeGuard(
      const RequestToken& token, std::unique_ptr<RequestData> data)
      : ShallowCopyRequestContextScopeGuard() {
    RequestContext::get()->overwriteContextData(token, std::move(data), true);
  }
  [[nodiscard]] ShallowCopyRequestContextScopeGuard(
      const std::string& val, std::unique_ptr<RequestData> data)
      : ShallowCopyRequestContextScopeGuard() {
    RequestContext::get()->overwriteContextData(val, std::move(data), true);
  }

  /**
   * Shallow copy then overwrite multiple RequestData instances
   *
   * Helper constructor which is more efficient than using multiple scope guards
   * Accepts iterators to a container of <string/RequestToken, RequestData
   * pointer> pairs
   */
  template <typename... Item>
  [[nodiscard]] explicit ShallowCopyRequestContextScopeGuard(
      RequestDataItem&& first, Item&&... rest)
      : ShallowCopyRequestContextScopeGuard(MultiTag{}, first, rest...) {}

  ~ShallowCopyRequestContextScopeGuard() {
    RequestContext::setContext(std::move(prev_));
  }

  ShallowCopyRequestContextScopeGuard(
      const ShallowCopyRequestContextScopeGuard&) = delete;
  ShallowCopyRequestContextScopeGuard& operator=(
      const ShallowCopyRequestContextScopeGuard&) = delete;
  ShallowCopyRequestContextScopeGuard(ShallowCopyRequestContextScopeGuard&&) =
      delete;
  ShallowCopyRequestContextScopeGuard& operator=(
      ShallowCopyRequestContextScopeGuard&&) = delete;

 private:
  struct MultiTag {};
  template <typename... Item>
  explicit ShallowCopyRequestContextScopeGuard(MultiTag, Item&... item)
      : ShallowCopyRequestContextScopeGuard() {
    auto rc = RequestContext::get();
    auto go = [&](RequestDataItem& i) {
      rc->overwriteContextData(i.first, std::move(i.second), true);
    };

    ((go(item)), ...);
  }

  std::shared_ptr<RequestContext> prev_;
};

// Debug-only guard that ensures that the current request context at destruction
// is the same as the one at construction.
class [[maybe_unused]] DCheckRequestContextRestoredGuard {
#ifndef NDEBUG
 public:
  [[nodiscard]] DCheckRequestContextRestoredGuard()
      : prev_(RequestContext::saveContext()) {}

  ~DCheckRequestContextRestoredGuard();

  DCheckRequestContextRestoredGuard(const DCheckRequestContextRestoredGuard&) =
      delete;
  DCheckRequestContextRestoredGuard& operator=(
      const DCheckRequestContextRestoredGuard&) = delete;
  DCheckRequestContextRestoredGuard(DCheckRequestContextRestoredGuard&&) =
      delete;
  DCheckRequestContextRestoredGuard& operator=(
      DCheckRequestContextRestoredGuard&&) = delete;

 private:
  std::shared_ptr<RequestContext> prev_;
#endif
};

template <class Traits>
/* static */ FOLLY_EXPORT RequestData*
RequestContext::getThreadCachedContextData() {
  thread_local RequestData* cachedData = nullptr;
  thread_local uint64_t cachedVersion = 0; // Allowed sentinel value.

  // In case cache is invalid, version snapshot must be taken before performing
  // the lookup.
  uint64_t curVersion = state_.version_.load(std::memory_order_acquire);

  if (curVersion == cachedVersion) {
    return cachedData;
  }

  cachedData = getContextData(Traits::kToken);
  cachedVersion = curVersion;
  return cachedData;
}

FOLLY_ALWAYS_INLINE uint64_t
RequestContext::getContextDataVersion() const noexcept {
  return state_.version_.load(std::memory_order_acquire);
}

} // namespace folly
