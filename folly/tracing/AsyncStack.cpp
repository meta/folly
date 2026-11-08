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

#include <folly/tracing/AsyncStack.h>

#include <atomic>
#include <cassert>
#include <exception>
#include <mutex>
#include <typeindex>
#include <unordered_set>

#include <glog/logging.h>
#include <glog/raw_logging.h>

#include <folly/Indestructible.h>
#include <folly/Likely.h>
#include <folly/Synchronized.h>
#include <folly/lang/Hint.h>

#if defined(__linux__)
#define FOLLY_ASYNC_STACK_ROOT_USE_PTHREAD 1
#else
#define FOLLY_ASYNC_STACK_ROOT_USE_PTHREAD 0
#endif

#if FOLLY_ASYNC_STACK_ROOT_USE_PTHREAD

#include <folly/portability/PThread.h>

// Use a global TLS key variable to make it easier for profilers/debuggers
// to lookup the current thread's AsyncStackRoot by walking the pthread
// TLS structures.
extern "C" {
// Current pthread implementation has valid keys in range 0 .. 1023.
// Initialise to some value that will be interpreted as an invalid key.
inline pthread_key_t folly_async_stack_root_tls_key = 0xFFFF'FFFFu;
}

#endif // FOLLY_ASYNC_STACK_ROOT_USE_PTHREAD

#if FOLLY_HAS_ASYNC_STACK_METADATA
extern "C" {
// `retain` is best-effort: assemblers predating SHF_GNU_RETAIN (e.g. the conda
// aarch64 toolchain) ignore it with -Wattributes, breaking -Werror builds.
// `used` alone still emits the symbol, so just silence the warning there.
FOLLY_PUSH_WARNING
FOLLY_GNU_DISABLE_WARNING("-Wattributes")
[[FOLLY_ATTR_GNU_USED, FOLLY_ATTR_GNU_RETAIN]]
FOLLY_EXPORT extern const std::uintptr_t
    folly_async_stack_metadata_frame_cookie = 0xDEADBEEFDEADBEEFULL;
FOLLY_POP_WARNING
}

// An out-of-process reader reports 0 when it cannot resolve the symbol, so 0
// must never be a valid cookie. Search for `read_folly_async_metadata_cookie`
// (Meta-internal).
static_assert(folly_async_stack_metadata_frame_cookie != 0);
#endif

namespace {
struct SuspendedFrameTag {};
} // namespace

extern "C" {
// AsyncFrames whose stackRoot is set to this value are considered to be
// "suspended" leaves. Debuggers may look up this symbol to
// identify suspended leaves
volatile uintptr_t __folly_suspended_frame_cookie{
    std::hash<std::type_index>{}(std::type_index(typeid(SuspendedFrameTag)))};

volatile bool __folly_instrumented_frame_tracking_enabled = folly::kIsDebug;

volatile std::unordered_set<folly::AsyncStackFrame*>* __folly_leaf_frame_store{
    nullptr};
}

namespace folly {

namespace {

#if FOLLY_ASYNC_STACK_ROOT_USE_PTHREAD
static pthread_once_t initialiseTlsKeyFlag = PTHREAD_ONCE_INIT;

static void ensureAsyncRootTlsKeyIsInitialised() noexcept {
  (void)pthread_once(&initialiseTlsKeyFlag, []() noexcept {
    int result = pthread_key_create(&folly_async_stack_root_tls_key, nullptr);
    if (result != 0) [[unlikely]] {
      RAW_LOG(
          FATAL,
          "Failed to initialise folly_async_stack_root_tls_key: (error: %d)",
          result);
      std::terminate();
    }
  });
}

#endif

struct AsyncStackRootHolder {
#if FOLLY_ASYNC_STACK_ROOT_USE_PTHREAD
  AsyncStackRootHolder() noexcept {
    ensureAsyncRootTlsKeyIsInitialised();
    const int result =
        pthread_setspecific(folly_async_stack_root_tls_key, this);
    if (result != 0) [[unlikely]] {
      RAW_LOG(
          FATAL,
          "Failed to set current thread's AsyncStackRoot: (error: %d)",
          result);
      std::terminate();
    }
  }
#endif

  AsyncStackRoot* get() const noexcept {
    return value.load(std::memory_order_relaxed);
  }

  void set(AsyncStackRoot* root) noexcept {
    std::atomic_signal_fence(std::memory_order_release);
    value.store(root, std::memory_order_relaxed);
  }

  void set_relaxed(AsyncStackRoot* root) noexcept {
    value.store(root, std::memory_order_relaxed);
  }

  std::atomic<AsyncStackRoot*> value{nullptr};
};

static AsyncStackRootHolder& getThreadAsyncStackRoot() noexcept {
  static thread_local AsyncStackRootHolder holder;
  return holder;
}

} // namespace

AsyncStackRoot* tryGetCurrentAsyncStackRoot() noexcept {
  return getThreadAsyncStackRoot().get();
}

AsyncStackRoot* exchangeCurrentAsyncStackRoot(
    AsyncStackRoot* newRoot) noexcept {
  auto* oldStackRoot = getThreadAsyncStackRoot().get();
  getThreadAsyncStackRoot().set(newRoot);
  return oldStackRoot;
}

namespace detail {

ScopedAsyncStackRoot::ScopedAsyncStackRoot(
    void* framePointer, void* returnAddress) noexcept {
  root_.setStackFrameContext(framePointer, returnAddress);
  root_.nextRoot = getThreadAsyncStackRoot().get();
  getThreadAsyncStackRoot().set(&root_);
}

ScopedAsyncStackRoot::~ScopedAsyncStackRoot() {
  assert(getThreadAsyncStackRoot().get() == &root_);
  assert(root_.topFrame.load(std::memory_order_relaxed) == nullptr);
  getThreadAsyncStackRoot().set_relaxed(root_.nextRoot);
}

} // namespace detail
} // namespace folly

namespace folly {

FOLLY_NOINLINE static void* get_return_address() noexcept {
  return FOLLY_ASYNC_STACK_RETURN_ADDRESS();
}

// This function is a special function that returns an address
// that can be used as a return-address and that will resolve
// debug-info to itself.
FOLLY_NOINLINE static void* detached_task() noexcept {
  void* p = get_return_address();

  // Add this after the call to prevent the compiler from
  // turning the call to get_return_address() into a tailcall.
  compiler_must_not_elide(p);

  return p;
}

AsyncStackRoot& getCurrentAsyncStackRoot() noexcept {
  auto* root = tryGetCurrentAsyncStackRoot();
  assert(root != nullptr);
  return *root;
}

AsyncStackFrame::AsyncStackFrame(MakeDetachedRootFrame) noexcept {
  setReturnAddress(detached_task());
}

AsyncStackFrame& getDetachedRootAsyncStackFrame() noexcept {
  static auto frame = AsyncStackFrame{AsyncStackFrame::MakeDetachedRootFrame{}};
  return frame;
}

#if FOLLY_HAS_COROUTINES

FOLLY_NOINLINE void resumeCoroutineWithNewAsyncStackRoot(
    coro::coroutine_handle<> h, folly::AsyncStackFrame& frame) noexcept {
  detail::ScopedAsyncStackRoot root;
  root.activateFrame(frame);
  h.resume();
}

#endif // FOLLY_HAS_COROUTINES

namespace {
auto& suspendedLeafFrames() {
  static folly::Indestructible<std::unique_ptr<
      folly::Synchronized<std::unordered_set<AsyncStackFrame*>>>>
      instance(folly::factory_constructor, []() {
        auto ret = std::make_unique<
            folly::Synchronized<std::unordered_set<AsyncStackFrame*>>>();
        __folly_leaf_frame_store = &ret->unsafeGetUnlocked();
        return ret;
      });
  return **instance;
}
} // namespace

void activateSuspendedLeaf(AsyncStackFrame& leafFrame) noexcept {
  assert(leafFrame.stackRoot == nullptr);
  leafFrame.stackRoot =
      reinterpret_cast<AsyncStackRoot*>(::__folly_suspended_frame_cookie);
  if constexpr (folly::kIsDebug) {
    suspendedLeafFrames().wlock()->insert(std::addressof(leafFrame));
  }
}

bool isSuspendedLeafActive(AsyncStackFrame& leafFrame) noexcept {
  return leafFrame.stackRoot ==
      reinterpret_cast<AsyncStackRoot*>(::__folly_suspended_frame_cookie);
}

void deactivateSuspendedLeaf(AsyncStackFrame& leafFrame) noexcept {
  assert(
      leafFrame.stackRoot ==
      reinterpret_cast<AsyncStackRoot*>(::__folly_suspended_frame_cookie));
  leafFrame.stackRoot = nullptr;
  if constexpr (folly::kIsDebug) {
    suspendedLeafFrames().wlock()->erase(std::addressof(leafFrame));
  }
}

void sweepSuspendedLeafFrames(folly::FunctionRef<void(AsyncStackFrame*)> fn) {
  suspendedLeafFrames().withRLock([&](auto& frames) {
    std::for_each(frames.begin(), frames.end(), fn);
  });
}

} // namespace folly
