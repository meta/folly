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
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

#include <folly/CPortability.h>
#include <folly/CppAttributes.h>
#include <folly/Function.h>
#include <folly/Portability.h>
#include <folly/Utility.h>
#include <folly/coro/Coroutine.h>
#include <folly/lang/Exception.h>
#include <folly/synchronization/AtomicRef.h>

// `AsyncStackMetadataFrame` stores a reserved value as an `AsyncStackFrame`
// return address. Its high bits do not match the sign-extension pattern
// required for an x86-64 virtual address, and the value is not
// instruction-aligned on AArch64.
//
// To add a 32-bit Linux target, reserve a permanent nonzero cookie below
// `folly::kMinValidAddress`, verify marker construction and the C++ readers
// under that ABI, and update out-of-process readers for its pointer width and
// frame layout.
#if UINTPTR_MAX == UINT64_MAX && \
    (FOLLY_X64 || FOLLY_AARCH64 || defined(_M_ARM64) || defined(_M_ARM64EC))
#define FOLLY_HAS_ASYNC_STACK_METADATA 1
#else
#define FOLLY_HAS_ASYNC_STACK_METADATA 0
#endif

#if FOLLY_HAS_ASYNC_STACK_METADATA
// `AsyncStackMetadataFrame` stores this permanent value with
// `AsyncStackFrame::setReturnAddress()`.
// External stack walkers may read this symbol from a shared library or an
// executable that retains symbol information. A fully stripped static
// executable may omit the name. If readers ever need to discover it there,
// export it into `.dynsym` at the final ELF link with
// `--export-dynamic-symbol=folly_async_stack_metadata_frame_cookie` (or the
// `-Wl,` form through a compiler driver); attributes and source references
// alone cannot make the name survive stripping. In fbsource, restore
// `xplat_linker_flags` forwarding in `fb_dirsync_cpp_library` so xplat can use
// the compiler-driver form while fbcode uses the raw linker flag. A future
// metadata-frame format must use another value.
extern "C" {
FOLLY_EXPORT extern const std::uintptr_t
    folly_async_stack_metadata_frame_cookie;
}
#endif

namespace folly {

// Gets the instruction pointer of the return-address of the current function.
//
// Generally a function that uses this macro should be declared FOLLY_NOINLINE
// to prevent this returning surprising results in cases where the function
// is inlined.
#if FOLLY_HAS_BUILTIN(__builtin_return_address)
#define FOLLY_ASYNC_STACK_RETURN_ADDRESS() __builtin_return_address(0)
#else
#define FOLLY_ASYNC_STACK_RETURN_ADDRESS() static_cast<void*>(nullptr)
#endif

// Gets pointer to the current function invocation's stack-frame.
//
// Generally a function that uses this macro should be declared FOLLY_NOINLINE
// to prevent this returning surprising results in cases where the function
// is inlined.
#if FOLLY_HAS_BUILTIN(__builtin_frame_address)
#define FOLLY_ASYNC_STACK_FRAME_POINTER() __builtin_frame_address(0)
#else
#define FOLLY_ASYNC_STACK_FRAME_POINTER() static_cast<void*>(nullptr)
#endif

// This header defines data-structures used to represent an async stack-trace.
//
// These data-structures are intended for use by coroutines (and possibly other
// representations of async operations) to allow the current program to record
// an async-stack as a linked-list of async-stack-frames in a similar way to
// how a normal thread represents the stack as a linked-list of stack frames.
//
// From a high-level, just looking at the AsyncStackRoot/AsyncStackFrame
// data-structures, each thread maintains a linked-list of active AsyncStack
// chains that looks a bit like this.
//
//  Stack Register
//      |
//      V
//  Stack Frame   currentStackRoot (TLS)
//      |               |
//      V               V
//  Stack Frame <- AsyncStackRoot  -> AsyncStackFrame -> AsyncStackFrame -> ...
//      |               |
//      V               |
//  Stack Frame         |
//      :               |
//      V               V
//  Stack Frame <- AsyncStackRoot  -> AsyncStackFrame -> AsyncStackFrame -> ...
//      |               |
//      V               X
//  Stack Frame
//      :
//      V
//
// Whenever a thread enters an event loop or is about to execute some
// asynchronus callback/continuation the current thread registers an
// AsyncStackRoot and records the stack-frame of the normal thread
// stack that corresponds to that call so that each AsyncStackRoot
// can be later interleaved with a normal thread stack-trace at
// the appropriate location.
//
// Each AsyncStackRoot contains a pointer to the currently active
// AsyncStackFrame (if any). This AsyncStackFrame forms the head
// of a linked-list of AsyncStackFrame objects that represent the
// async stack-trace. Each non-head AsyncStackFrame is a suspended
// asynchronous operation, which is typically suspended waiting for
// the previous operation to complete.
//
//
// The following diagram shows in more detail how each of the fields
// in these data-structures relate to each other and how the
// async-stack interleaves with the normal thread-stack.
//
//      Current Thread Stack
//      ====================
// +------------------------------------+ <--- current top of stack
// | Normal Stack Frame                 |
// | - stack-base-pointer  ---.         |
// | - return-address         |         |          Thread Local Storage
// |                          |         |          ====================
// +--------------------------V---------+
// |         ...                        |     +-------------------------+
// |                          :         |     | - currentStackRoot  -.  |
// |                          :         |     |                      |  |
// +--------------------------V---------+     +----------------------|--+
// | Normal Stack Frame                 |                            |
// | - stack-base-pointer  ---.         |                            |
// | - return-address         |      .-------------------------------`
// |                          |      |  |
// +--------------------------V------|--+
// | Active Async Operation          |  |
// | (Callback or Coroutine)         |  |            Heap Allocated
// | - stack-base-pointer  ---.      |  |            ==============
// | - return-address         |      |  |
// | - pointer to async state | --------------> +-------------------------+
// |   (e.g. coro frame or    |      |  |       | Coroutine Frame         |
// |    future core)          |      |  |       | +---------------------+ |
// |                          |      |  |       | | Promise             | |
// +--------------------------V------|--+       | | +-----------------+ | |
// |   Event  / Callback             |  |   .------>| AsyncStackFrame | | |
// |   Loop     Callsite             |  |   |   | | | - parentFrame  --------.
// | - stack-base-pointer  ---.      |  |   |   | | | - instructionPtr| | |  |
// | - return-address         |      |  |   |   | | | - stackRoot -.  | | |  |
// |                          |      |  |   |   | | +--------------|--+ | |  |
// |  +--------------------+  |      |  |   |   | | ...            |    | |  |
// |  | AsyncStackRoot     |<--------`  |   |   | +----------------|----+ |  |
// |  | - topFrame   -----------------------`   | ...              |      |  |
// |  | - stackFramePtr -. |<---------------,   +------------------|------+  |
// |  | - nextRoot --.   | |  |         |   |                      |         |
// |  +--------------|---|-+  |         |   '----------------------`         |
// +-----------------|---V----V---------+       +-------------------------+  |
// |         ...     |                  |       | Coroutine Frame         |  |
// |                 |        :         |       |                         |  |
// |                 |        :         |       |  +-------------------+  |  |
// +-----------------|--------V---------+       |  | AsyncStackFrame   |<----`
// | Async Operation |                  |       |  | - parentFrame   --------.
// | (Callback/Coro) |                  |       |  | - instructionPtr  |  |  |
// |                 |        :         |       |  | - stackRoot       |  |  |
// |                 |        :         |       |  +-------------------+  |  |
// +-----------------|--------V---------+       +-------------------------+  |
// |  Event Loop /   |                  |                                    :
// |  Callback Call  |                  |                                    :
// | - frame-pointer | -------.         |                                    V
// | - return-address|        |         |
// |                 |        |         |      Another chain of potentially
// |  +--------------V-----+  |         |      unrelated AsyncStackFrame
// |  | AsyncStackRoot     |  |         |       +---------------------+
// |  | - topFrame  ---------------- - - - - >  | AsyncStackFrame     |
// |  | - stackFramePtr -. |  |         |       | - parentFrame -.    |
// |  | - nextRoot -.    | |  |         |       +----------------|----+
// |  +-------------|----|-+  |         |                        :
// |                |    |    |         |                        V
// +----------------|----V----V---------+
// |         ...    :                   |
// |                V                   |
// |                                    |
// +------------------------------------+
//
//
// This data-structure can be inspected from within the current process
// if desired, but is also intended to allow tools such as debuggers or
// profilers that are inspecting the memory of this process remotely.

struct AsyncStackRoot;
struct AsyncStackFrame;
class AsyncStackMetadata;
namespace detail {
class ScopedAsyncStackRoot;
AsyncStackMetadata getAsyncStackTraceEntryMetadata(
    const AsyncStackFrame& frame) noexcept;
} // namespace detail

// Get access to the current thread's top-most AsyncStackRoot.
//
// Returns nullptr if there is no active AsyncStackRoot.
[[nodiscard]] AsyncStackRoot* tryGetCurrentAsyncStackRoot() noexcept;

// Get access to the current thread's top-most AsyncStackRoot.
//
// Assumes that there is a current AsyncStackRoot.
[[nodiscard]] AsyncStackRoot& getCurrentAsyncStackRoot() noexcept;

// Exchange the current thread's active AsyncStackRoot with the
// specified AsyncStackRoot pointer, returning the old AsyncStackRoot
// pointer.
//
// This is intended to be used to update the thread-local pointer
// when context-switching fiber stacks.
[[nodiscard]] AsyncStackRoot* exchangeCurrentAsyncStackRoot(
    AsyncStackRoot* newRoot) noexcept;

// Perform some consistency checks on the specified AsyncStackFrame,
// assuming that it is the currently active AsyncStackFrame.
void checkAsyncStackFrameIsActive(const folly::AsyncStackFrame& frame) noexcept;

// Activate the specified AsyncStackFrame on the specified AsyncStackRoot,
// setting it as the current 'topFrame'.
//
// The AsyncStackRoot must be the current thread's top-most AsyncStackRoot
// and it must not currently have an active 'topFrame'.
//
// This is typically called immediately prior to executing a callback that
// resumes the async operation represented by 'frame'.
void activateAsyncStackFrame(
    folly::AsyncStackRoot& root, folly::AsyncStackFrame& frame) noexcept;

// Deactivate the specified AsyncStackFrame, clearing the current 'topFrame'.
//
// Typically called when the current async operation completes or is suspended
// and execution is about to return from the callback to the executor's event
// loop.
void deactivateAsyncStackFrame(folly::AsyncStackFrame& frame) noexcept;

// Push the 'callee' frame onto the current thread's async stack, deactivating
// the 'caller' frame and setting up the 'caller' to be the parent-frame of
// the 'callee'.
//
// The 'caller' frame must be the current thread's active frame.
//
// After this call, the 'callee' frame will be the current thread's active
// frame.
//
// This is typically used when one async operation is about to transfer
// execution to a child async operation. e.g. via a coroutine symmetric
// transfer.
void pushAsyncStackFrameCallerCallee(
    folly::AsyncStackFrame& callerFrame,
    folly::AsyncStackFrame& calleeFrame) noexcept;

// Pop the 'callee' frame off the stack, restoring the parent frame as the
// current frame.
//
// This is typically used when the current async operation completes and
// you are about to call/resume the caller. e.g. performing a symmetric
// transfer to the calling coroutine in final_suspend().
//
// If calleeFrame.getParentFrameUnsafe() is null then this method is equivalent
// to deactivateAsyncStackFrame(), leaving no active AsyncStackFrame on
// the current AsyncStackRoot.
void popAsyncStackFrameCallee(folly::AsyncStackFrame& calleeFrame) noexcept;

// Get a pointer to a special frame that can be used as the root-frame
// for a chain of AsyncStackFrame that does not chain onto a normal
// call-stack.
//
// The caller should never modify this frame as it will be shared across
// many frames and threads. The implication of this restriction is that
// you should also never activate this frame.
AsyncStackFrame& getDetachedRootAsyncStackFrame() noexcept;

// Given an initial AsyncStackFrame, this will write `addresses` with
// the return addresses of the frames in this async stack trace, up to
// `maxAddresses` written.
// Metadata marker frames are omitted and do not count against `maxAddresses`.
// The walk stops if two metadata marker frames are adjacent.
// This assumes `addresses` has `maxAddresses` allocated space available.
// Returns the number of frames written.
size_t getAsyncStackTraceFromInitialFrame(
    folly::AsyncStackFrame* initialFrame,
    std::uintptr_t* addresses,
    size_t maxAddresses);

// Run the same walk. If `metadata` is not null, write one metadata value beside
// each address: `metadata[i]` describes `addresses[i]`, and unannotated frames
// receive `std::nullopt`.
// This assumes `addresses` and any non-null `metadata` each have
// `maxAddresses` allocated space available.
size_t getAsyncStackTraceFromInitialFrameWithMetadata(
    folly::AsyncStackFrame* initialFrame,
    std::uintptr_t* addresses,
    AsyncStackMetadata* metadata,
    size_t maxAddresses);

#if FOLLY_HAS_COROUTINES

// Resume the specified coroutine after installing a new AsyncStackRoot
// on the current thread and setting the specified AsyncStackFrame as
// the current async frame.
FOLLY_NOINLINE void resumeCoroutineWithNewAsyncStackRoot(
    coro::coroutine_handle<> h, AsyncStackFrame& frame) noexcept;

// Resume the specified coroutine after installing a new AsyncStackRoot
// on the current thread and setting the coroutine's associated
// AsyncStackFrame, obtained by calling promise.getAsyncFrame(), as the
// current async frame.
template <typename Promise>
void resumeCoroutineWithNewAsyncStackRoot(
    coro::coroutine_handle<Promise> h) noexcept;

#endif // FOLLY_HAS_COROUTINES

/**
 * Push a dummy "leaf" frame into the stack to annotate the stack as
 * "suspended".
 *
 * The leaf frame will be made discoverable to debugging tools
 * which may use the leaves to walk the stack traces of suspended stacks.
 */
void activateSuspendedLeaf(AsyncStackFrame& leafFrame) noexcept;

bool isSuspendedLeafActive(AsyncStackFrame& leafFrame) noexcept;

/**
 * Apply `fn` on all suspended leaf frames.
 * Note: Avoid performing async work within `fn` as it may cause deadlocks.
 *
 * This API does nothing in non-debug-builds.
 */
void sweepSuspendedLeafFrames(folly::FunctionRef<void(AsyncStackFrame*)> fn);

/**
 * Pop the dummy "leaf" frame off the stack to annotate the stack as
 * having resumed.
 *
 * The leaf frame will no longer be discoverable to debugging tools
 */
void deactivateSuspendedLeaf(AsyncStackFrame& leafFrame) noexcept;

// An async stack frame contains information about a particular
// invocation of an asynchronous operation.
//
// For example, asynchronous operations implemented using coroutines
// would have each coroutine-frame contain an instance of AsyncStackFrame
// to record async-stack trace information for that coroutine invocation.
//
// Reader lifetime contract: async-stack readers do not keep frames alive. A
// signal handler may walk the thread it interrupted because that thread cannot
// change or destroy frames until the handler returns. An external reader must
// stop the owning thread before walking it. A separately running thread must
// not unlink or destroy a reachable frame during a walk; supporting that would
// require a way to keep frames alive.
struct AsyncStackFrame {
 public:
  AsyncStackFrame() = default;
  ~AsyncStackFrame();
  AsyncStackFrame(const AsyncStackFrame&) = delete;
  AsyncStackFrame(AsyncStackFrame&&) = delete;
  AsyncStackFrame& operator=(const AsyncStackFrame&) = delete;
  AsyncStackFrame& operator=(AsyncStackFrame&&) = delete;

  // Call only after this frame is no longer reachable from an AsyncStackRoot.
  void clear() noexcept {
    parentFrame = nullptr;
    instructionPointer = nullptr;
    stackRoot = nullptr;
  }

  // The parent frame is the frame of the async operation that is logically
  // the caller of this frame. It is null when this frame has no logical async
  // caller.
  //
  // Ordinary frames set this link before a walker can see the frame and leave
  // it unchanged while visible. Metadata is different: it temporarily rewires
  // an active frame, so a signal handler can read the link during the update.
  // On metadata-capable targets, the unsuffixed methods use atomic accesses
  // so a signal handler on the same thread sees either the old or new link.
  // The reader lifetime contract above still applies.
  AsyncStackFrame* getParentFrame() noexcept { return loadParentFrame(); }
  const AsyncStackFrame* getParentFrame() const noexcept {
    return loadParentFrame();
  }
  void setParentFrame(AsyncStackFrame& frame) noexcept {
    storeParentFrame(&frame);
  }
  void clearParentFrame() noexcept { storeParentFrame(nullptr); }

  // These methods perform plain pointer accesses. A write requires that no
  // asynchronous stack walker can reach this frame during the call; a read
  // requires that no writer can change the link.
  AsyncStackFrame* getParentFrameUnsafe() noexcept { return parentFrame; }
  const AsyncStackFrame* getParentFrameUnsafe() const noexcept {
    return parentFrame;
  }
  void setParentFrameUnsafe(AsyncStackFrame& frame) noexcept {
    parentFrame = &frame;
  }

  // Get access to the current stack-root.
  //
  // This is only valid for either the root or leaf AsyncStackFrame
  // in a chain of frames.
  //
  // In the case of an active leaf-frame it is used as a cache to
  // avoid accessing the thread-local when pushing/popping frames.
  // In the case of the root frame (which has a null parent frame)
  // it points to an AsyncStackRoot that contains information about
  // the normal-stack caller.
  AsyncStackRoot* getStackRoot() noexcept;

  // The return address is generallty the address of the code in the
  // caller that will be executed when the operation owning the current
  // frame completes.
  void setReturnAddress(void* p = FOLLY_ASYNC_STACK_RETURN_ADDRESS()) noexcept;
  void* getReturnAddress() const noexcept;

 private:
  struct MakeDetachedRootFrame {};
  explicit AsyncStackFrame(MakeDetachedRootFrame) noexcept;

  friend AsyncStackRoot;

  friend AsyncStackFrame& getDetachedRootAsyncStackFrame() noexcept;
  friend void activateAsyncStackFrame(
      folly::AsyncStackRoot&, folly::AsyncStackFrame&) noexcept;
  friend void deactivateAsyncStackFrame(folly::AsyncStackFrame&) noexcept;
  friend void pushAsyncStackFrameCallerCallee(
      folly::AsyncStackFrame&, folly::AsyncStackFrame&) noexcept;
  friend void checkAsyncStackFrameIsActive(
      const folly::AsyncStackFrame&) noexcept;
  friend void popAsyncStackFrameCallee(folly::AsyncStackFrame&) noexcept;

  friend void activateSuspendedLeaf(folly::AsyncStackFrame&) noexcept;
  friend bool isSuspendedLeafActive(folly::AsyncStackFrame&) noexcept;
  friend void deactivateSuspendedLeaf(AsyncStackFrame& leafFrame) noexcept;

  // Pointer to the async caller's stack-frame info.
  //
  // This forms a linked-list of frames that make up a stack.
  // The list is terminated by a null pointer which indicates
  // the top of the async stack - either because the operation
  // is detached or because the next frame is a thread that is
  // blocked waiting for the async stack to complete.
#if FOLLY_HAS_ASYNC_STACK_METADATA
  // folly::atomic_ref delegates its operations to std::atomic<T>.
  static_assert(std::atomic<AsyncStackFrame*>::is_always_lock_free);
  static_assert(
      alignof(AsyncStackFrame*) >=
      folly::atomic_ref<AsyncStackFrame*>::required_alignment);
#endif
  // `mutable` lets the const synchronized getter form an `atomic_ref` without
  // changing the pointer's debugger-visible representation.
  mutable AsyncStackFrame* parentFrame = nullptr;

  AsyncStackFrame* loadParentFrame() const noexcept {
#if FOLLY_HAS_ASYNC_STACK_METADATA
    auto* frame = folly::atomic_ref<AsyncStackFrame*>{parentFrame}.load(
        std::memory_order_relaxed);
    // For a same-thread signal walk, keep reads through `frame` after this
    // relaxed load. This fence only constrains compiler reordering.
    std::atomic_signal_fence(std::memory_order_acquire);
    return frame;
#else
    return parentFrame;
#endif
  }

  void storeParentFrame(AsyncStackFrame* frame) noexcept {
#if FOLLY_HAS_ASYNC_STACK_METADATA
    // For a same-thread signal walk, keep earlier parent initialization before
    // the relaxed store. This fence only constrains compiler reordering.
    std::atomic_signal_fence(std::memory_order_release);
    folly::atomic_ref<AsyncStackFrame*>{parentFrame}.store(
        frame, std::memory_order_relaxed);
    // The lifetime contract above excludes concurrent destruction. In
    // `~AsyncStackMetadataFrame()`, this only keeps the compiler from moving
    // the unlinked frame's destruction or storage reuse before the relaxed
    // store.
    std::atomic_signal_fence(std::memory_order_acquire);
#else
    parentFrame = frame;
#endif
  }

  // Instruction pointer of the caller of this frame.
  // This will typically be either the address of the continuation
  // of this asynchronous operation, or the address of the code
  // that launched this asynchronous operation. May be null
  // if the address is not known.
  //
  // Typically initialised with the result of a call to
  // FOLLY_ASYNC_STACK_RETURN_ADDRESS().
  void* instructionPointer = nullptr;

  // Pointer to the stack-root for the current thread.
  // Cache this in each async-stack frame so we don't have to
  // read from a thread-local to get the pointer.
  //
  // This pointer is only valid for the top-most stack frame.
  // When a frame is pushed or popped it should be copied to
  // the next frame, etc.
  //
  // The exception is for the bottom-most frame (ie. where
  // parentFrame == null). In this case, if stackRoot is non-null
  // then it points to a root that is currently blocked on some
  // thread waiting for the async work to complete. In this case
  // you can find the information about the stack-frame for that
  // thread in the AsyncStackRoot and can use it to continue
  // walking the stack-frames.
  AsyncStackRoot* stackRoot = nullptr;
};

// A stack-root represents the context of an event loop
// that is running some asynchronous work. The current async
// operation that is being executed by the event loop (if any)
// is pointed to by the 'topFrame'.
//
// If the current event loop is running nested inside some other
// event loop context then the 'nextRoot' points to the AsyncStackRoot
// context for the next event loop up the stack on the current thread.
//
// The 'stackFramePtr' holds a pointer to the normal stack-frame
// that is currently executing this event loop. This allows
// reconciliation of the parts between a normal stack-trace and
// the start of the async-stack trace.
//
// The current thread's top-most context (the head of the linked
// list of contexts) is obtained by calling getCurrentAsyncStackRoot().
struct AsyncStackRoot {
 public:
  AsyncStackRoot() = default;
  ~AsyncStackRoot() = default;
  AsyncStackRoot(const AsyncStackRoot&) = delete;
  AsyncStackRoot(AsyncStackRoot&&) = delete;
  AsyncStackRoot& operator=(const AsyncStackRoot&) = delete;
  AsyncStackRoot& operator=(AsyncStackRoot&&) = delete;

  // Sets the top-frame to be 'frame' and also updates the cached
  // 'frame.stackRoot' to be 'this'.
  //
  // The current stack root must not currently have any active
  // frame.
  void setTopFrame(AsyncStackFrame& frame) noexcept;
  AsyncStackFrame* getTopFrame() const noexcept;

  // Initialises this stack root with information about the context
  // in which the stack-root was declared. This records information
  // about where the async-stack-trace should be spliced into the
  // normal stack-trace.
  void setStackFrameContext(
      void* framePtr = FOLLY_ASYNC_STACK_FRAME_POINTER(),
      void* ip = FOLLY_ASYNC_STACK_RETURN_ADDRESS()) noexcept;
  void* getStackFramePointer() const noexcept;
  void* getReturnAddress() const noexcept;

  const AsyncStackRoot* getNextRoot() const noexcept;
  void setNextRoot(AsyncStackRoot* next) noexcept;

 private:
  friend class detail::ScopedAsyncStackRoot;
  friend void activateAsyncStackFrame(
      folly::AsyncStackRoot&, folly::AsyncStackFrame&) noexcept;
  friend void deactivateAsyncStackFrame(folly::AsyncStackFrame&) noexcept;
  friend void pushAsyncStackFrameCallerCallee(
      folly::AsyncStackFrame&, folly::AsyncStackFrame&) noexcept;
  friend void checkAsyncStackFrameIsActive(
      const folly::AsyncStackFrame&) noexcept;
  friend void popAsyncStackFrameCallee(folly::AsyncStackFrame&) noexcept;

  // Pointer to the currently-active AsyncStackFrame for this event
  // loop or callback invocation. May be null if this event loop is
  // not currently executing any async operations.
  //
  // Atomic access does not keep the frame alive. See the reader lifetime
  // contract above `AsyncStackFrame`.
  std::atomic<AsyncStackFrame*> topFrame{nullptr};

  // Pointer to the next event loop context lower on the current
  // thread's stack.
  // This is nullptr if this is not a nested call to an event loop.
  AsyncStackRoot* nextRoot = nullptr;

  // Pointer to the stack-frame and return-address of the function
  // call that registered this AsyncStackRoot on the current thread.
  // This is generally the stack-frame responsible for executing async
  // callbacks (typically an event-loop).
  // Anything prior to this frame on the stack in the current thread
  // is potentially unrelated to the call-chain of the current async-stack.
  //
  // Typically initialised with FOLLY_ASYNC_STACK_FRAME_POINTER() or
  // setStackFrameContext().
  void* stackFramePtr = nullptr;

  // Typically initialise with FOLLY_ASYNC_STACK_RETURN_ADDRESS() or
  // setStackFrameContext().
  void* returnAddress = nullptr;
};

/**
 * The metadata attached to one frame of an async stack trace, if any.
 *
 * This optional-like type keeps the representation under Folly's control for
 * profiler-facing arrays instead of depending on a library's std::optional
 * layout. It is not a stable C++ ABI or wire format; an out-of-process payload
 * decoder needs an explicit versioned contract. Presence is a separate flag
 * rather than a reserved payload value because zero is valid metadata.
 */
class AsyncStackMetadata {
 public:
  using value_type = std::uintptr_t;

  constexpr AsyncStackMetadata() noexcept : value_{0}, has_value_{false} {}

  /* implicit */ constexpr AsyncStackMetadata(std::nullopt_t) noexcept
      : value_{0}, has_value_{false} {}
  /* implicit */ constexpr AsyncStackMetadata(value_type value) noexcept
      : value_{value}, has_value_{true} {}

  constexpr AsyncStackMetadata& operator=(std::nullopt_t) noexcept {
    reset();
    return *this;
  }
  constexpr AsyncStackMetadata& operator=(value_type value) noexcept {
    emplace(value);
    return *this;
  }

  constexpr const value_type& emplace(value_type value) noexcept {
    value_ = value;
    has_value_ = true;
    return value_;
  }

  constexpr void reset() noexcept {
    value_ = 0;
    has_value_ = false;
  }

  constexpr void swap(AsyncStackMetadata& that) noexcept {
    AsyncStackMetadata tmp = *this;
    *this = that;
    that = tmp;
  }

  constexpr bool has_value() const noexcept { return has_value_; }
  constexpr explicit operator bool() const noexcept { return has_value(); }

  constexpr const value_type& operator*() const noexcept {
    assert(has_value());
    return value_;
  }
  constexpr const value_type* operator->() const noexcept {
    assert(has_value());
    return &value_;
  }

  constexpr const value_type* get_pointer() const noexcept {
    return has_value() ? &value_ : nullptr;
  }

  constexpr value_type value() const {
    if (!has_value()) {
      throw_exception<std::bad_optional_access>();
    }
    return value_;
  }

  constexpr value_type value_or(value_type fallback) const noexcept {
    return has_value() ? value_ : fallback;
  }

  friend constexpr bool operator==(
      const AsyncStackMetadata&, const AsyncStackMetadata&) noexcept = default;

  friend constexpr void swap(
      AsyncStackMetadata& a, AsyncStackMetadata& b) noexcept {
    a.swap(b);
  }

 private:
  value_type value_;
  bool has_value_;
};

static_assert(std::is_trivially_copyable_v<AsyncStackMetadata>);
static_assert(std::is_standard_layout_v<AsyncStackMetadata>);

namespace detail {

inline bool isAsyncStackMetadataFrame(const AsyncStackFrame& frame) noexcept {
#if FOLLY_HAS_ASYNC_STACK_METADATA
  return reinterpret_cast<std::uintptr_t>(frame.getReturnAddress()) ==
      folly_async_stack_metadata_frame_cookie;
#else
  // This build cannot create metadata markers through the supported API.
  (void)frame;
  return false;
#endif
}

} // namespace detail

#if FOLLY_HAS_ASYNC_STACK_METADATA
// An async-stack marker that carries metadata rather than a code address. It is
// temporarily spliced into the parent chain after an ordinary frame. Readers
// identify it by `folly_async_stack_metadata_frame_cookie` and omit it from
// address traces. Only one such scope may annotate a frame at a time; overlap
// would put two markers next to each other, which readers reject as invalid.
// Do not otherwise change the annotated frame's parent while this object lives.
class AsyncStackMetadataFrame : private NonCopyableNonMovable {
 public:
  AsyncStackMetadataFrame(
      AsyncStackFrame& frame, std::uintptr_t metadata) noexcept
      : metadataFrame_{}, metadata_{metadata}, frame_{&frame} {
    metadataFrame_.setReturnAddress(
        // NOLINTNEXTLINE(performance-no-int-to-ptr): reserved marker value
        reinterpret_cast<void*>(folly_async_stack_metadata_frame_cookie));
    auto* parent = frame.getParentFrame();
    assert(parent == nullptr || !detail::isAsyncStackMetadataFrame(*parent));
    if (parent != nullptr) {
      metadataFrame_.setParentFrameUnsafe(*parent);
    }

    frame.setParentFrame(metadataFrame_);
  }

  ~AsyncStackMetadataFrame() {
    assert(frame_->getParentFrame() == &metadataFrame_);
    if (auto* parent = metadataFrame_.getParentFrame()) {
      frame_->setParentFrame(*parent);
    } else {
      frame_->clearParentFrame();
    }
  }

 private:
  static_assert(std::is_standard_layout_v<AsyncStackFrame>);
  friend AsyncStackMetadata detail::getAsyncStackTraceEntryMetadata(
      const AsyncStackFrame&) noexcept;

  // Readers recover this object from the marker pointer, so this frame must
  // remain the first data member.
  AsyncStackFrame metadataFrame_;
  // A published marker's payload is immutable because an interrupted reader
  // has no protocol for observing an in-place update consistently.
  AsyncStackMetadata metadata_;
  AsyncStackFrame* frame_;
};

// A standard-layout object has the same address as its first data member.
static_assert(std::is_standard_layout_v<AsyncStackMetadataFrame>);

#endif

namespace detail {

struct MetadataFrameAdvanceResult {
  AsyncStackFrame* frame;
  bool foundAdjacentMetadataFrame;
};

// Two adjacent metadata frames mean the chain is invalid; normal metadata
// construction never creates them. Skip one metadata frame, if present, and
// report a second one so callers can stop before a marker cycle loops forever
// without filling the output buffer.
[[nodiscard]] inline MetadataFrameAdvanceResult advancePastMetadataFrame(
    AsyncStackFrame* frame) noexcept {
  if (frame == nullptr || !isAsyncStackMetadataFrame(*frame)) {
    return {frame, false};
  }
  frame = frame->getParentFrame();
  return {frame, frame != nullptr && isAsyncStackMetadataFrame(*frame)};
}

inline AsyncStackMetadata getAsyncStackTraceEntryMetadata(
    const AsyncStackFrame& frame) noexcept {
#if FOLLY_HAS_ASYNC_STACK_METADATA
  // A running child stores the return address of the wrapper that awaits it.
  // For `child -> wrapper -> marker`, annotate that saved address.
  auto* wrapperFrame = frame.getParentFrame();
  // A `co_withMetadata` wrapper marks its children. If `frame` is also a
  // wrapper, its metadata comes from the outer wrapper, not its own marker.
  if (wrapperFrame != nullptr && isAsyncStackMetadataFrame(*wrapperFrame)) {
    wrapperFrame = wrapperFrame->getParentFrame();
  }
  // No wrapper means no metadata; adjacent metadata frames are invalid.
  if (wrapperFrame == nullptr || isAsyncStackMetadataFrame(*wrapperFrame)) {
    return std::nullopt;
  }

  const auto* metadataFrame = wrapperFrame->getParentFrame();
  if (metadataFrame == nullptr || !isAsyncStackMetadataFrame(*metadataFrame)) {
    return std::nullopt;
  }

  // A marker in a supported frame chain is the embedded first member of a live
  // `AsyncStackMetadataFrame`.
  return reinterpret_cast<const AsyncStackMetadataFrame*>(metadataFrame)
      ->metadata_;
#else
  (void)frame;
  return std::nullopt;
#endif
}

class ScopedAsyncStackRoot {
 public:
  explicit ScopedAsyncStackRoot(
      void* framePointer = FOLLY_ASYNC_STACK_FRAME_POINTER(),
      void* returnAddress = FOLLY_ASYNC_STACK_RETURN_ADDRESS()) noexcept;
  ~ScopedAsyncStackRoot();

  void activateFrame(AsyncStackFrame& frame) noexcept {
    folly::activateAsyncStackFrame(root_, frame);
  }

 private:
  AsyncStackRoot root_;
};

} // namespace detail

inline size_t getAsyncStackTraceFromInitialFrameWithMetadata(
    folly::AsyncStackFrame* initialFrame,
    std::uintptr_t* addresses,
    AsyncStackMetadata* metadata,
    size_t maxAddresses) {
  size_t numFrames = 0;
  auto* frame = initialFrame;
  while (frame != nullptr && numFrames < maxAddresses) {
    const auto advanceResult = detail::advancePastMetadataFrame(frame);
    frame = advanceResult.frame;
    if (frame == nullptr || advanceResult.foundAdjacentMetadataFrame) {
      break;
    }

    addresses[numFrames] =
        reinterpret_cast<std::uintptr_t>(frame->getReturnAddress());
    if (metadata != nullptr) {
      metadata[numFrames] = detail::getAsyncStackTraceEntryMetadata(*frame);
    }
    ++numFrames;
    // The output is full; reading the unused parent link may fault.
    if (numFrames == maxAddresses) {
      break;
    }
    frame = frame->getParentFrame();
  }
  return numFrames;
}

inline size_t getAsyncStackTraceFromInitialFrame(
    folly::AsyncStackFrame* initialFrame,
    std::uintptr_t* addresses,
    size_t maxAddresses) {
  return getAsyncStackTraceFromInitialFrameWithMetadata(
      initialFrame, addresses, nullptr, maxAddresses);
}

} // namespace folly

#include <folly/tracing/AsyncStack-inl.h>
