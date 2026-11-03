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

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <folly/FBString.h>
#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/String.h>
#include <folly/debugging/symbolizer/Dwarf.h>
#include <folly/debugging/symbolizer/ElfCache.h>
#include <folly/debugging/symbolizer/StackTrace.h>
#include <folly/debugging/symbolizer/SymbolCache.h>
#include <folly/debugging/symbolizer/SymbolizePrinter.h>
#include <folly/debugging/symbolizer/SymbolizedFrame.h>
#include <folly/io/IOBuf.h>
#include <folly/portability/Config.h>
#include <folly/portability/Unistd.h>

namespace folly {
namespace symbolizer {

/**
 * Get stack trace into a given FrameArray, return true on success (and
 * set frameCount to the actual frame count, which may be > N) and false
 * on failure.
 */
namespace detail {
template <size_t N>
bool fixFrameArray(FrameArray<N>& fa, ssize_t n) {
  if (n != -1) {
    fa.frameCount = n;
    for (size_t i = 0; i < fa.frameCount; ++i) {
      fa.frames[i].found = false;
    }
    return true;
  } else {
    fa.frameCount = 0;
    return false;
  }
}

std::string getStackTraceStr(
    const uintptr_t* addresses,
    SymbolizedFrame* frames,
    size_t frameCount,
    bool showFullInfo,
    size_t skip = 0);
} // namespace detail

// Always inline these functions; they don't do much, and unittests rely
// on them never showing up in a stack trace.
template <size_t N>
FOLLY_ALWAYS_INLINE bool getStackTrace(FrameArray<N>& fa);

template <size_t N>
inline bool getStackTrace(FrameArray<N>& fa) {
  return detail::fixFrameArray(fa, getStackTrace(fa.addresses, N));
}
template <size_t N>
FOLLY_ALWAYS_INLINE bool getStackTraceSafe(FrameArray<N>& fa);

template <size_t N>
inline bool getStackTraceSafe(FrameArray<N>& fa) {
  return detail::fixFrameArray(fa, getStackTraceSafe(fa.addresses, N));
}

template <size_t N>
FOLLY_ALWAYS_INLINE bool getStackTraceHeap(FrameArray<N>& fa);

template <size_t N>
inline bool getStackTraceHeap(FrameArray<N>& fa) {
  return detail::fixFrameArray(fa, getStackTraceHeap(fa.addresses, N));
}

template <size_t N>
FOLLY_ALWAYS_INLINE bool getAsyncStackTraceSafe(FrameArray<N>& fa);

template <size_t N>
inline bool getAsyncStackTraceSafe(FrameArray<N>& fa) {
  return detail::fixFrameArray(fa, getAsyncStackTraceSafe(fa.addresses, N));
}

#if FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

class Symbolizer {
 public:
  static constexpr auto kDefaultLocationInfoMode = LocationInfoMode::FAST;

  static bool isAvailable();

  explicit Symbolizer(LocationInfoMode mode = kDefaultLocationInfoMode)
      : Symbolizer(nullptr, mode) {}

  explicit Symbolizer(
      ElfCacheBase* cache,
      LocationInfoMode mode = kDefaultLocationInfoMode,
      size_t symbolCacheSize = 0,
      std::string exePath = "/proc/self/exe");

  /**
   * Symbolize through a cache owned by the caller, so that it can be shared
   * with other Symbolizers. It must outlive this object, and must only be
   * shared with Symbolizers using the same `mode` and `cache`; see
   * SymbolCacheBase.
   */
  Symbolizer(
      ElfCacheBase* cache,
      LocationInfoMode mode,
      SymbolCacheBase& symbolCache,
      std::string exePath = "/proc/self/exe");

  ~Symbolizer();

  /**
   *  Symbolize given addresses and return the number of @frames filled:
   *
   * - all entries in @addrs will be symbolized (if possible, e.g. if they're
   *   valid code addresses and if frames.size() >= addrs.size())
   *
   * - if `mode_ == FULL_WITH_INLINE` and `frames.size() > addrs.size()` then at
   *   most `frames.size() - addrs.size()` additional inlined functions will
   *   also be symbolized (at most `kMaxInlineLocationInfoPerFrame` per @addr
   *   entry).
   */
  size_t symbolize(
      folly::Range<const uintptr_t*> addrs,
      folly::Range<SymbolizedFrame*> frames);

  size_t symbolize(
      const uintptr_t* addresses, SymbolizedFrame* frames, size_t frameCount) {
    return symbolize(
        folly::Range<const uintptr_t*>(addresses, frameCount),
        folly::Range<SymbolizedFrame*>(frames, frameCount));
  }

  template <size_t N>
  size_t symbolize(FrameArray<N>& fa) {
    return symbolize(
        folly::Range<const uintptr_t*>(fa.addresses, fa.frameCount),
        folly::Range<SymbolizedFrame*>(fa.frames, N));
  }

  /**
   * Shortcut to symbolize one address.
   */
  bool symbolize(uintptr_t address, SymbolizedFrame& frame) {
    symbolize(
        folly::Range<const uintptr_t*>(&address, 1),
        folly::Range<SymbolizedFrame*>(&frame, 1));
    return frame.found;
  }

 private:
  ElfCacheBase* const cache_;
  const LocationInfoMode mode_;
  const std::string exePath_;
  const std::unique_ptr<SymbolCacheBase> ownedSymbolCache_;
  SymbolCacheBase* const symbolCache_;
};

/**
 * Use this class to print a stack trace from normal code.  It will malloc and
 * won't flush or sync.
 *
 * These methods are thread safe, through locking.  However, they are not
 * signal safe.
 */
class FastStackTracePrinter {
 public:
  static constexpr size_t kDefaultSymbolCacheSize = 10000;

  explicit FastStackTracePrinter(
      std::unique_ptr<SymbolizePrinter> printer,
      size_t symbolCacheSize = kDefaultSymbolCacheSize);

  ~FastStackTracePrinter();

  /**
   * This is NOINLINE to make sure it shows up in the stack we grab, which
   * makes it easy to skip printing it.
   */
  FOLLY_NOINLINE void printStackTrace(bool symbolize);

  void flush();

 private:
  static constexpr size_t kMaxStackTraceDepth = 100;

  const std::unique_ptr<SymbolizePrinter> printer_;
  Symbolizer symbolizer_;
};

/**
 * This is a copy of FastStackTracePrinter, but it uses a two-step approach to
 * symbolize the stack trace. This is useful for cases where symbolization is
 * slow and we want to avoid blocking the main thread.
 */
class TwoStepFastStackTracePrinter {
 public:
  static constexpr size_t kDefaultSymbolCacheSize = 10000;

  explicit TwoStepFastStackTracePrinter(
      std::unique_ptr<SymbolizePrinter> printer,
      size_t symbolCacheSize = kDefaultSymbolCacheSize);

  FOLLY_NOINLINE void printStackTrace(bool symbolize);

 private:
  static constexpr size_t kMaxStackTraceDepth = 100;

  const std::unique_ptr<SymbolizePrinter> printer_;
  Symbolizer symbolizer_;
  FrameArray<kMaxStackTraceDepth> syncAddresses_;
  FrameArray<kMaxStackTraceDepth> asyncAddresses_;
  std::mutex mutex_;
};

#endif // FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

/**
 * Use this class to print a stack trace from a signal handler, or other place
 * where you shouldn't allocate memory on the heap, and fsync()ing your file
 * descriptor is more important than performance.
 *
 * Make sure to create one of these on startup, not in the signal handler, as
 * the constructor allocates on the heap, whereas the other methods don't.
 * Best practice is to just leak this object, rather than worry about
 * destruction order.
 *
 * These methods aren't thread safe, so if you could have signals on multiple
 * threads at the same time, you need to do your own locking to ensure you
 * don't call these methods from multiple threads.  They are signal safe,
 * however.
 */
class SafeStackTracePrinter {
 public:
  explicit SafeStackTracePrinter(int fd = STDERR_FILENO);

  virtual ~SafeStackTracePrinter() {}

  /**
   * Only allocates on the stack and is signal-safe but not thread-safe. Don't
   * call printStackTrace() on the same StackTracePrinter object from multiple
   * threads at the same time.
   *
   * This is NOINLINE to make sure it shows up in the stack we grab, which
   * makes it easy to skip printing it.
   */
  FOLLY_NOINLINE void printStackTrace(bool symbolize);

  void print(StringPiece sp) { printer_.print(sp); }

  // Flush printer_, also fsync, in case we're about to crash again...
  void flush();

  // Install one process-lifetime observer before this printer is used.
  bool setOutputSink(FDSymbolizePrinter::OutputSink sink) {
    return printer_.setOutputSink(sink);
  }

 protected:
  virtual void printSymbolizedStackTrace();
  void printUnsymbolizedStackTrace();

 private:
  static constexpr size_t kMaxStackTraceDepth = 100;

  int fd_;
  FDSymbolizePrinter printer_;
  std::unique_ptr<FrameArray<kMaxStackTraceDepth>> addresses_;
};

#if FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

/**
 * Gets the stack trace for the current thread, skipping the top `skip` frames,
 * and returns a string representation. Convenience function meant for
 * debugging and logging. Empty string indicates stack trace functionality
 * is not available.
 *
 * NOT async-signal-safe.
 * @param showFullInfo If true, include file names and line numbers (when
 * available).
 * @param skip Skip the top `skip` frames.
 */
std::string getStackTraceStr(bool showFullInfo = false, size_t skip = 0);

/**
 * Gets the async stack trace for the current thread, skipping the top `skip`
 * frames, and returns a string representation. Convenience function meant for
 * debugging and logging. Empty string indicates stack trace functionality
 * is not available.
 *
 * NOT async-signal-safe.
 */
std::string getAsyncStackTraceStr(size_t skip = 0);

/**
 * Get the async stack traces (string representation) for suspended
 * coroutines. Convenience function meant for debugging and logging, works
 * only in some DEBUG builds
 *
 * Note: The returned traces will only have async frames (no normal frames).
 */
std::vector<std::string> getSuspendedStackTraces();

#else
// Define these in the header, as headers are always available, but not all
// platforms can link against the symbolizer library cpp sources.

inline std::string getStackTraceStr(
    bool /*showFullInfo*/ = false, size_t /*skip*/ = 0) {
  return "";
}

inline std::string getAsyncStackTraceStr(size_t /*skip*/ = 0) {
  return "";
}

inline std::vector<std::string> getSuspendedStackTraces() {
  return {};
}

namespace detail {
inline std::string getStackTraceStr(
    const uintptr_t* /*addresses*/,
    SymbolizedFrame* /*frames*/,
    size_t /*frameCount*/,
    bool /*showFullInfo*/,
    size_t /*skip*/) {
  return "";
}
} // namespace detail
#endif // FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

/**
 * Convert FrameArray to string representation.
 * Convenience function for debugging and logging.
 */
template <size_t N>
FOLLY_ALWAYS_INLINE std::string getStackTraceStr(
    FrameArray<N>& fa, bool showFullInfo = false, size_t skip = 0);

template <size_t N>
inline std::string getStackTraceStr(
    FrameArray<N>& fa, bool showFullInfo, size_t skip) {
  return detail::getStackTraceStr(
      fa.addresses, fa.frames, fa.frameCount, showFullInfo, skip);
}

#if FOLLY_HAVE_SWAPCONTEXT

/**
 * Use this class in rare situations where signal handlers are running in a
 * tiny stack specified by sigaltstack.
 *
 * This is neither thread-safe nor signal-safe. However, it can usually print
 * something useful while SafeStackTracePrinter would stack overflow.
 *
 * Signal handlers would need to block other signals to make this safer.
 * Note it's still unsafe even with that.
 */
class UnsafeSelfAllocateStackTracePrinter : public SafeStackTracePrinter {
 public:
  /// Reserve the emergency stack and eagerly resolve context-switch bindings.
  /// Construct before installing handlers: binding resolution is not
  /// async-signal-safe.
  explicit UnsafeSelfAllocateStackTracePrinter(int fd = STDERR_FILENO);
  ~UnsafeSelfAllocateStackTracePrinter() override;

 protected:
  void printSymbolizedStackTrace() override;

 private:
  struct AllocatedStack;

  static std::unique_ptr<AllocatedStack> allocateStack();
  static void contextStart(UnsafeSelfAllocateStackTracePrinter* that);
  bool runOnAllocatedStack();

  const std::unique_ptr<AllocatedStack> stack_;
};

#endif // FOLLY_HAVE_SWAPCONTEXT

} // namespace symbolizer
} // namespace folly
