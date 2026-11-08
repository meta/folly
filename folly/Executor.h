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
#include <cassert>
#include <climits>
#include <string_view>
#include <utility>

#include <folly/Function.h>
#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/Utility.h>
#include <folly/lang/Exception.h>

//  Compiles in the keep-alive object trace points below. Off by default: this
//  header reaches nearly every translation unit, so the trace points cost
//  binary size in every binary in the repo even with no hooks installed. Set it
//  for the whole build, never for a subset of targets -- the trace points sit
//  in inline and template functions whose out-of-line copies are comdat-folded
//  across translation units, so a mixed build silently gets whichever copy the
//  linker keeps.
#ifndef FOLLY_ENABLE_KEEPALIVE_OBJ_TRACE
#define FOLLY_ENABLE_KEEPALIVE_OBJ_TRACE 0
#endif

namespace folly {

using Func = Function<void()>;

class Executor;

namespace detail {

class ExecutorKeepAliveBase {
 public:
  //  A dummy keep-alive is a keep-alive to an executor which does not support
  //  the keep-alive mechanism.
  static constexpr uintptr_t kDummyFlag = uintptr_t(1) << 0;

  //  An alias keep-alive is a keep-alive to an executor to which there is
  //  known to be another keep-alive whose lifetime surrounds the lifetime of
  //  the alias.
  static constexpr uintptr_t kAliasFlag = uintptr_t(1) << 1;

  static constexpr uintptr_t kFlagMask = kDummyFlag | kAliasFlag;
  static constexpr uintptr_t kExecutorMask = ~kFlagMask;
};

//  Optional, process-global tracing hooks for keep-alive ownership, keyed by
//  the address of the ExecutorKeepAlive object itself rather than by the
//  executor it refers to. Keying on the instance is what makes a release
//  pairable with its acquire: a move re-keys the existing record instead of
//  looking like a release plus an unrelated acquire, so the recorded set is
//  the exact set of live keep-alives rather than a biased sample of them.
//
//  Only keep-alives which hold an executor reference count are acquired and
//  released: dummy and alias keep-alives hold none, and are skipped on exactly
//  the same condition ExecutorKeepAlive::reset() uses to decide whether to call
//  keepAliveRelease(). The move hook is deliberately not filtered that way: it
//  fires for every move, so `from` may be an address no acquire ever reported.
//  A hook must read that as "`to` holds no counted reference either" and drop
//  any record it holds for `to` -- which is also what clears a record stranded
//  at a recycled address.
//
//  This is a debug facility. Without FOLLY_ENABLE_KEEPALIVE_OBJ_TRACE the trace
//  points compile away and the keep-alive path is unchanged. With it, and with
//  no hooks installed, each trace point is a relaxed load of a never-written
//  global and a FOLLY_UNLIKELY branch. With hooks installed it costs whatever
//  the hook costs on every acquire, release and move process-wide -- install
//  only for a diagnostic run. The hooks run from arbitrary threads: they must
//  be fast and must not throw.
using KeepAliveObjTraceHook = void (*)(const void* obj, Executor* target);
using KeepAliveObjMoveHook = void (*)(const void* from, const void* to);

//  Function-local statics rather than out-of-line globals: this header is
//  included nearly everywhere, and a global defined in Executor.cpp would give
//  every translation unit that instantiates a keep-alive an undefined symbol
//  unless it also took a link-time dependency on Executor.cpp. std::atomic's
//  constructor is constexpr, so these are constant-initialized and carry no
//  thread-safe-static guard: the load below stays a plain relaxed load.
FOLLY_EXPORT FOLLY_ALWAYS_INLINE std::atomic<KeepAliveObjTraceHook>&
keepAliveObjAcquireHook() {
  static std::atomic<KeepAliveObjTraceHook> hook{nullptr};
  return hook;
}

FOLLY_EXPORT FOLLY_ALWAYS_INLINE std::atomic<KeepAliveObjTraceHook>&
keepAliveObjReleaseHook() {
  static std::atomic<KeepAliveObjTraceHook> hook{nullptr};
  return hook;
}

FOLLY_EXPORT FOLLY_ALWAYS_INLINE std::atomic<KeepAliveObjMoveHook>&
keepAliveObjMoveHook() {
  static std::atomic<KeepAliveObjMoveHook> hook{nullptr};
  return hook;
}

inline void traceKeepAliveObjAcquire(
    [[maybe_unused]] const void* obj,
    [[maybe_unused]] Executor* target) noexcept {
#if FOLLY_ENABLE_KEEPALIVE_OBJ_TRACE
  auto hook = keepAliveObjAcquireHook().load(std::memory_order_relaxed);
  if (hook != nullptr) [[unlikely]] {
    hook(obj, target);
  }
#endif
}

inline void traceKeepAliveObjRelease(
    [[maybe_unused]] const void* obj,
    [[maybe_unused]] Executor* target) noexcept {
#if FOLLY_ENABLE_KEEPALIVE_OBJ_TRACE
  auto hook = keepAliveObjReleaseHook().load(std::memory_order_relaxed);
  if (hook != nullptr) [[unlikely]] {
    hook(obj, target);
  }
#endif
}

inline void traceKeepAliveObjMove(
    [[maybe_unused]] const void* from,
    [[maybe_unused]] const void* to) noexcept {
#if FOLLY_ENABLE_KEEPALIVE_OBJ_TRACE
  auto hook = keepAliveObjMoveHook().load(std::memory_order_relaxed);
  if (hook != nullptr) [[unlikely]] {
    hook(from, to);
  }
#endif
}

} // namespace detail

//  Install (or, with all-null arguments, remove) the keep-alive object tracing
//  hooks described above. Not synchronized with in-flight keep-alive
//  operations: install once at startup, before the threads being traced exist.
//  Returns false when FOLLY_ENABLE_KEEPALIVE_OBJ_TRACE is unset: the hooks are
//  stored, but no trace point will ever call them.
bool setKeepAliveObjTraceHooks(
    detail::KeepAliveObjTraceHook acquire,
    detail::KeepAliveObjTraceHook release,
    detail::KeepAliveObjMoveHook move);

/**
 * `ExecutorKeepAlive` is a safe pointer to an `Executor`.
 *
 * For any `Executor` that supports keep-alive functionality, its destructor
 * will block until all the `ExecutorKeepAlive` objects associated with that
 * executor are destroyed.  For executors that don't support the keep-alive
 * functionality, `ExecutorKeepAlive` doesn't provide such protection.
 *
 * `ExecutorKeepAlive` should *always* be used instead of `Executor*`.
 * `ExecutorKeepAlive` can be implicitly constructed from `Executor*`.
 *
 * The `getKeepAliveToken()` helper can be used to construct a keep-alive in
 * templated code if you need to preserve the original executor type.
 */
template <typename ExecutorT = Executor>
class ExecutorKeepAlive : private detail::ExecutorKeepAliveBase {
 public:
  using KeepAliveFunc = Function<void(ExecutorKeepAlive&&)>;

  ExecutorKeepAlive() = default;

  ~ExecutorKeepAlive() {
    static_assert(
        std::is_standard_layout<ExecutorKeepAlive>::value, "standard-layout");
    static_assert(sizeof(ExecutorKeepAlive) == sizeof(void*), "pointer size");
    static_assert(
        alignof(ExecutorKeepAlive) == alignof(void*), "pointer align");

    reset();
  }

  ExecutorKeepAlive(ExecutorKeepAlive&& other) noexcept
      : storage_(std::exchange(other.storage_, 0)) {
    detail::traceKeepAliveObjMove(&other, this);
  }

  ExecutorKeepAlive(const ExecutorKeepAlive& other) noexcept;

  template <
      typename OtherExecutor,
      typename = typename std::enable_if<
          std::is_convertible<OtherExecutor*, ExecutorT*>::value>::type>
  /* implicit */ ExecutorKeepAlive(
      ExecutorKeepAlive<OtherExecutor>&& other) noexcept
      : ExecutorKeepAlive(other.get(), other.storage_ & kFlagMask) {
    other.storage_ = 0;
    detail::traceKeepAliveObjMove(&other, this);
  }

  template <
      typename OtherExecutor,
      typename = typename std::enable_if<
          std::is_convertible<OtherExecutor*, ExecutorT*>::value>::type>
  /* implicit */ ExecutorKeepAlive(
      const ExecutorKeepAlive<OtherExecutor>& other) noexcept;

  /* implicit */ ExecutorKeepAlive(ExecutorT* executor);

  ExecutorKeepAlive& operator=(ExecutorKeepAlive&& other) noexcept {
    //  reset() first, so the release of the overwritten value is traced before
    //  the move re-keys the incoming one onto this address.
    reset();
    storage_ = std::exchange(other.storage_, 0);
    detail::traceKeepAliveObjMove(&other, this);
    return *this;
  }

  ExecutorKeepAlive& operator=(ExecutorKeepAlive const& other) {
    return operator=(folly::copy(other));
  }

  template <
      typename OtherExecutor,
      typename = typename std::enable_if<
          std::is_convertible<OtherExecutor*, ExecutorT*>::value>::type>
  ExecutorKeepAlive& operator=(
      ExecutorKeepAlive<OtherExecutor>&& other) noexcept {
    return *this = ExecutorKeepAlive(std::move(other));
  }

  template <
      typename OtherExecutor,
      typename = typename std::enable_if<
          std::is_convertible<OtherExecutor*, ExecutorT*>::value>::type>
  ExecutorKeepAlive& operator=(const ExecutorKeepAlive<OtherExecutor>& other) {
    return *this = ExecutorKeepAlive(other);
  }

  void reset() noexcept;

  explicit operator bool() const { return storage_; }

  ExecutorT* get() const {
    return reinterpret_cast<ExecutorT*>(storage_ & kExecutorMask);
  }

  ExecutorT& operator*() const { return *get(); }

  ExecutorT* operator->() const { return get(); }

  ExecutorKeepAlive copy() const;

  ExecutorKeepAlive get_alias() const {
    return ExecutorKeepAlive(storage_ | kAliasFlag);
  }

  template <class KAF>
  void add(KAF&& f) && {
    static_assert(
        is_invocable<KAF, ExecutorKeepAlive&&>::value,
        "Parameter to add must be void(ExecutorKeepAlive&&)>");
    auto ex = get();
    ex->add([ka = std::move(*this), f_2 = std::forward<KAF>(f)]() mutable {
      f_2(std::move(ka));
    });
  }

 private:
  friend class Executor;
  template <typename OtherExecutor>
  friend class ExecutorKeepAlive;

  ExecutorKeepAlive(ExecutorT* executor, uintptr_t flags) noexcept
      : storage_(reinterpret_cast<uintptr_t>(executor) | flags) {
    assert(executor);
    assert(!(reinterpret_cast<uintptr_t>(executor) & ~kExecutorMask));
    assert(!(flags & kExecutorMask));
  }

  explicit ExecutorKeepAlive(uintptr_t storage) noexcept : storage_(storage) {}

  //  Combined storage for the executor pointer and for all flags.
  uintptr_t storage_{reinterpret_cast<uintptr_t>(nullptr)};
};

/// An Executor accepts units of work with add(), which should be
/// threadsafe.
class Executor {
 public:
  virtual ~Executor() = default;

  /// Enqueue a function to be executed by this executor. This and all
  /// variants must be threadsafe.
  virtual void add(Func) = 0;

  /// Enqueue a function with a given priority, where 0 is the medium priority
  /// This is up to the implementation to enforce
  virtual void addWithPriority(Func, int8_t priority);

  virtual uint8_t getNumPriorities() const { return 1; }

  static constexpr int8_t LO_PRI = SCHAR_MIN;
  static constexpr int8_t MID_PRI = 0;
  static constexpr int8_t HI_PRI = SCHAR_MAX;

  // Compatibility shim.  Cannot be forward-declared, unlike
  // `ExecutorKeepAlive`.
  template <typename ExecutorT = Executor>
  using KeepAlive = ExecutorKeepAlive<ExecutorT>;

  template <typename ExecutorT>
  static KeepAlive<ExecutorT> getKeepAliveToken(ExecutorT* executor) {
    static_assert(
        std::is_base_of<Executor, ExecutorT>::value,
        "getKeepAliveToken only works for folly::Executor implementations.");
    if (!executor) {
      return {};
    }
    folly::Executor* executorPtr = executor;
    if (executorPtr->keepAliveAcquire()) {
      return makeKeepAlive<ExecutorT>(executor);
    }
    return makeKeepAliveDummy<ExecutorT>(executor);
  }

  template <typename ExecutorT>
  static KeepAlive<ExecutorT> getKeepAliveToken(ExecutorT& executor) {
    static_assert(
        std::is_base_of<Executor, ExecutorT>::value,
        "getKeepAliveToken only works for folly::Executor implementations.");
    return getKeepAliveToken(&executor);
  }

  template <typename F>
  FOLLY_ERASE static void invokeCatchingExns(char const* p, F f) noexcept {
    catch_exception(f, invokeCatchingExnsLog, p);
  }

 protected:
  template <typename>
  friend class ExecutorKeepAlive;

  /**
   * Returns true if the KeepAlive is constructed from an executor that does
   * not support the keep alive ref-counting functionality
   */
  template <typename ExecutorT>
  static bool isKeepAliveDummy(const KeepAlive<ExecutorT>& keepAlive) {
    return keepAlive.storage_ & KeepAlive<ExecutorT>::kDummyFlag;
  }

  static bool keepAliveAcquire(Executor* executor) {
    return executor->keepAliveAcquire();
  }
  static void keepAliveRelease(Executor* executor) {
    return executor->keepAliveRelease();
  }

  // Acquire a keep alive token. Should return false if keep-alive mechanism
  // is not supported.
  virtual bool keepAliveAcquire() noexcept;
  // Release a keep alive token previously acquired by keepAliveAcquire().
  // Will never be called if keepAliveAcquire() returns false.
  virtual void keepAliveRelease() noexcept;

  //  This is the sole origin of a reference-counting (non-dummy, non-alias)
  //  keep-alive: every such keep-alive is either made here or moved from one
  //  that was, so tracing the acquire here -- rather than in the individual
  //  ExecutorKeepAlive constructors, all of which funnel through
  //  getKeepAliveToken() -- pairs one acquire with the one release in reset().
  //  The captured stack still names the original caller, since the
  //  constructors are simply further up the same stack.
  template <typename ExecutorT>
  static KeepAlive<ExecutorT> makeKeepAlive(ExecutorT* executor) {
    static_assert(
        std::is_base_of<Executor, ExecutorT>::value,
        "makeKeepAlive only works for folly::Executor implementations.");
    KeepAlive<ExecutorT> keepAlive{executor, uintptr_t(0)};
    detail::traceKeepAliveObjAcquire(&keepAlive, executor);
    return keepAlive;
  }

 private:
  static void invokeCatchingExnsLog(char const* prefix) noexcept;

  template <typename ExecutorT>
  static KeepAlive<ExecutorT> makeKeepAliveDummy(ExecutorT* executor) {
    static_assert(
        std::is_base_of<Executor, ExecutorT>::value,
        "makeKeepAliveDummy only works for folly::Executor implementations.");
    return KeepAlive<ExecutorT>{executor, KeepAlive<ExecutorT>::kDummyFlag};
  }
};

template <typename ExecutorT>
ExecutorKeepAlive<ExecutorT>::ExecutorKeepAlive(
    const ExecutorKeepAlive<ExecutorT>& other) noexcept
    : ExecutorKeepAlive(Executor::getKeepAliveToken(other.get())) {}

template <typename ExecutorT>
template <typename OtherExecutor, typename>
ExecutorKeepAlive<ExecutorT>::ExecutorKeepAlive(
    const ExecutorKeepAlive<OtherExecutor>& other) noexcept
    : ExecutorKeepAlive(Executor::getKeepAliveToken(other.get())) {}

template <typename ExecutorT>
ExecutorKeepAlive<ExecutorT>::ExecutorKeepAlive(ExecutorT* executor) {
  *this = Executor::getKeepAliveToken(executor);
}

template <typename ExecutorT>
void ExecutorKeepAlive<ExecutorT>::reset() noexcept {
  if (Executor* executor = get()) {
    auto const flags = std::exchange(storage_, 0) & kFlagMask;
    if (!(flags & (kDummyFlag | kAliasFlag))) {
      detail::traceKeepAliveObjRelease(this, executor);
      executor->keepAliveRelease();
    }
  }
}

template <typename ExecutorT>
ExecutorKeepAlive<ExecutorT> ExecutorKeepAlive<ExecutorT>::copy() const {
  return Executor::isKeepAliveDummy(*this) //
      ? Executor::makeKeepAliveDummy(get())
      : Executor::getKeepAliveToken(get());
}

/// Returns a keep-alive token which guarantees that Executor will keep
/// processing tasks until the token is released (if supported by Executor).
/// KeepAlive always contains a valid pointer to an Executor.
template <typename ExecutorT>
Executor::KeepAlive<ExecutorT> getKeepAliveToken(ExecutorT* executor) {
  static_assert(
      std::is_base_of<Executor, ExecutorT>::value,
      "getKeepAliveToken only works for folly::Executor implementations.");
  return Executor::getKeepAliveToken(executor);
}

template <typename ExecutorT>
Executor::KeepAlive<ExecutorT> getKeepAliveToken(ExecutorT& executor) {
  static_assert(
      std::is_base_of<Executor, ExecutorT>::value,
      "getKeepAliveToken only works for folly::Executor implementations.");
  return getKeepAliveToken(&executor);
}

template <typename ExecutorT>
Executor::KeepAlive<ExecutorT> getKeepAliveToken(
    Executor::KeepAlive<ExecutorT>& ka) {
  return ka.copy();
}

struct ExecutorBlockingContext {
  bool forbid;
  bool allowTerminationOnBlocking;
  Executor* ex = nullptr;
  std::string_view tag;
};
static_assert(
    std::is_standard_layout<ExecutorBlockingContext>::value,
    "non-standard layout");

struct ExecutorBlockingList {
  ExecutorBlockingList* prev;
  ExecutorBlockingContext curr;
};
static_assert(
    std::is_standard_layout<ExecutorBlockingList>::value,
    "non-standard layout");

class ExecutorBlockingGuard {
 public:
  struct PermitTag {};
  struct TrackTag {};
  struct ProhibitTag {};

  ~ExecutorBlockingGuard();
  ExecutorBlockingGuard() = delete;

  explicit ExecutorBlockingGuard(PermitTag) noexcept;
  explicit ExecutorBlockingGuard(
      TrackTag, Executor* ex, std::string_view tag) noexcept;
  explicit ExecutorBlockingGuard(
      ProhibitTag, Executor* ex, std::string_view tag) noexcept;

  ExecutorBlockingGuard(ExecutorBlockingGuard&&) = delete;
  ExecutorBlockingGuard(ExecutorBlockingGuard const&) = delete;

  ExecutorBlockingGuard& operator=(ExecutorBlockingGuard const&) = delete;
  ExecutorBlockingGuard& operator=(ExecutorBlockingGuard&&) = delete;

 private:
  ExecutorBlockingList list_;
};

Optional<ExecutorBlockingContext> getExecutorBlockingContext() noexcept;

} // namespace folly
