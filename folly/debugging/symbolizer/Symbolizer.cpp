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

#include <folly/debugging/symbolizer/Symbolizer.h>

#include <cstdlib>

#include <folly/FileUtil.h>
#include <folly/Memory.h>
#include <folly/ScopeGuard.h>
#include <folly/debugging/symbolizer/Dwarf.h>
#include <folly/debugging/symbolizer/Elf.h>
#include <folly/debugging/symbolizer/ElfCache.h>
#include <folly/debugging/symbolizer/SymbolCache.h>
#include <folly/debugging/symbolizer/detail/Debug.h>
#include <folly/lang/SafeAssert.h>
#include <folly/lang/ToAscii.h>
#include <folly/memory/SanitizeAddress.h>
#include <folly/portability/Config.h>
#include <folly/portability/SysMman.h>
#include <folly/portability/Unistd.h>
#include <folly/small_vector.h>
#include <folly/tracing/AsyncStack.h>

#if FOLLY_HAVE_SWAPCONTEXT
// folly/portability/Config.h (thus features.h) must be included
// first, and _XOPEN_SOURCE must be defined to unable the context
// functions on macOS.
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif
#include <ucontext.h>
#endif

#if FOLLY_HAVE_BACKTRACE
#include <execinfo.h>
#endif

#ifdef __roar__
extern "C" char* __roar_api_symbolize(
    void* Address, unsigned* NumFrames, bool WithSrcLine, bool WithInline);
extern "C" int __roar_api_address_is_code(void* Address);
#endif

namespace folly {
namespace symbolizer {

namespace {
template <typename PrintFunc>
void printAsyncStackInfo(PrintFunc print) {
  char buf[to_ascii_size_max<16, uint64_t>];
  auto printHex = [&print, &buf](uint64_t val) {
    print("0x");
    print(StringPiece(buf, to_ascii_lower<16>(buf, val)));
  };

  // Print async stack trace, if available
  const auto* asyncStackRoot = tryGetCurrentAsyncStackRoot();
  const auto* asyncStackFrame =
      asyncStackRoot ? asyncStackRoot->getTopFrame() : nullptr;

  print("\n");
  print("*** Check failure async stack trace: ***\n");
  print("*** First async stack root: ");
  printHex((uint64_t)asyncStackRoot);
  print(", normal stack frame pointer holding async stack root: ");
  printHex(
      asyncStackRoot ? (uint64_t)asyncStackRoot->getStackFramePointer() : 0);
  print(", return address: ");
  printHex(asyncStackRoot ? (uint64_t)asyncStackRoot->getReturnAddress() : 0);
  print(" ***\n");
  print("*** First async stack frame pointer: ");
  printHex((uint64_t)asyncStackFrame);
  print(", return address: ");
  printHex(asyncStackFrame ? (uint64_t)asyncStackFrame->getReturnAddress() : 0);
  print(", async stack trace: ***\n");
}

} // namespace

#if FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

namespace {

template <size_t N>
void symbolizeAndPrint(
    const std::unique_ptr<SymbolizePrinter>& printer,
    Symbolizer& symbolizer,
    FrameArray<N>& addresses,
    bool symbolize) {
  if (symbolize) {
    symbolizer.symbolize(addresses);

    // Skip the top 2 frames:
    printer->println(addresses, 2);
  } else {
    printer->print("(safe mode, symbolizer not available)\n");
    AddressFormatter formatter;
    for (size_t i = 0; i < addresses.frameCount; ++i) {
      printer->print(formatter.format(addresses.addresses[i]));
      printer->print("\n");
    }
  }
}

ElfCache* defaultElfCache() {
  static auto cache = new ElfCache();
  return cache;
}

#ifdef __roar__
bool setROARSymbolizedFrame(
    SymbolizedFrame& frame,
    uintptr_t address,
    LocationInfoMode mode,
    folly::Range<SymbolizedFrame*> extraInlineFrames = {}) {
  const bool withSrcLine = mode != LocationInfoMode::DISABLED;
  const bool withInline = mode == LocationInfoMode::FULL_WITH_INLINE;
  unsigned numFrames = 0;
  char* jitNames = __roar_api_symbolize(
      reinterpret_cast<void*>(address), &numFrames, withSrcLine, withInline);
  if (numFrames == 0)
    return false;
  unsigned firstFrame =
      numFrames - std::min<unsigned>(numFrames, extraInlineFrames.size() + 1);
  unsigned i = 0;
  for (unsigned curFrame = 0; curFrame < numFrames; ++curFrame) {
    std::string_view name = std::string_view(jitNames);
    jitNames += name.size() + 1;
    std::string_view fileName = std::string_view(jitNames);
    jitNames += fileName.size() + 1;
    std::string_view lineNo = std::string_view(jitNames);
    jitNames += lineNo.size() + 1;
    if (curFrame < firstFrame) {
      continue;
    }
    if (curFrame == numFrames - 1) {
      frame.name = name.data();
      frame.location.hasFileAndLine = withSrcLine && !fileName.empty();
      frame.location.file = Path({}, {}, fileName);
      frame.location.line = atoi(lineNo.data());
      break;
    }
    extraInlineFrames[i].found = true;
    extraInlineFrames[i].addr = address;
    extraInlineFrames[i].name = name.data();
    extraInlineFrames[i].location.hasFileAndLine =
        withSrcLine && !fileName.empty();
    extraInlineFrames[i].location.file = Path({}, {}, fileName);
    extraInlineFrames[i].location.line = atoi(lineNo.data());
    ++i;
  }
  return true;
}

#endif

void setSymbolizedFrame(
    ElfCacheBase* const elfCache,
    SymbolizedFrame& frame,
    const std::shared_ptr<ElfFile>& file,
    uintptr_t address,
    LocationInfoMode mode,
    folly::Range<SymbolizedFrame*> extraInlineFrames = {}) {
  frame.clear();
  frame.found = true;
  frame.addr = address;
  frame.file = file;
#ifdef __roar__
  if (setROARSymbolizedFrame(frame, address, mode, extraInlineFrames)) {
    return;
  }
#endif
  frame.name = file->getSymbolName(file->getDefinitionByAddress(address));

  Dwarf(elfCache, file.get())
      .findAddress(address, mode, frame, extraInlineFrames);
}

/**
 * @param instructionAddr The address of an instruction after it has been
 * adjusted by the linker's `l_addr`.
 * @return true if the given address is contained in an executable segment of
 * `elfFile`.
 */
bool containedInExecutableSegment(
    const ElfFile& elfFile, ElfAddr instructionAddr) {
#ifdef __roar__
  // In ROAR, JITed code might be loaded at an address that was not marked as
  // executable in the original ELF file. In this case, ask ROAR if the address
  // is code or not.
  if (__roar_api_address_is_code(reinterpret_cast<void*>(instructionAddr))) {
    return true;
  }
#endif
  return elfFile.iterateProgramHeaders([&](const ElfPhdr& sh) {
    bool executable = sh.p_flags & PF_X;
    bool loadable = sh.p_type == PT_LOAD;
    if (!(executable && loadable)) {
      return false;
    }
    return sh.p_vaddr <= instructionAddr &&
        instructionAddr < (sh.p_vaddr + sh.p_memsz);
  });
}

// Number of leading frames that were filled in.
size_t countFrames(folly::Range<SymbolizedFrame*> frames) {
  return std::distance(
      frames.begin(),
      std::find_if(frames.begin(), frames.end(), [](const SymbolizedFrame& f) {
        return !f.found;
      }));
}

// Symbolize `adjusted` (an ELF-relative address) in `elfFile`, resolving up to
// `maxInline` inlined calls. Returns the inline frames followed by the
// non-inlined call, i.e. the layout SymbolCache stores.
CachedSymbolizedFrames symbolizeAddress(
    ElfCacheBase* const elfCache,
    const std::shared_ptr<ElfFile>& elfFile,
    uintptr_t adjusted,
    LocationInfoMode mode,
    size_t maxInline) {
  CachedSymbolizedFrames result;
  result.reserve(maxInline + 1);
  result.resize(maxInline);
  auto const inlineFrames = folly::range(result);

  SymbolizedFrame callerFrame;
  setSymbolizedFrame(
      elfCache, callerFrame, elfFile, adjusted, mode, inlineFrames);

  // Drop the inline slots that were not filled in; the caller goes last.
  result.resize(countFrames(inlineFrames));
  result.push_back(std::move(callerFrame));
  return result;
}

// Splice `src` into `frames` at `index`, shifting the frames in
// [index + 1, addrCount) right to make room for its inline frames. Returns the
// number of inline frames inserted.
size_t spliceFrames(
    folly::Range<SymbolizedFrame*> frames,
    size_t index,
    size_t addrCount,
    const CachedSymbolizedFrames& src) {
  size_t const numInlined = src.size() - 1;
  if (numInlined != 0) {
    std::move_backward(
        frames.begin() + index + 1,
        frames.begin() + addrCount,
        frames.begin() + addrCount + numInlined);
  }
  std::copy(src.begin(), src.end(), frames.begin() + index);
  return numInlined;
}

} // namespace

bool Symbolizer::isAvailable() {
  return detail::get_r_debug();
}

Symbolizer::Symbolizer(
    ElfCacheBase* cache,
    LocationInfoMode mode,
    size_t symbolCacheSize,
    std::string exePath)
    : cache_(cache ? cache : defaultElfCache()),
      mode_(mode),
      exePath_(std::move(exePath)),
      ownedSymbolCache_(
          symbolCacheSize > 0 ? makeLruSymbolCache(symbolCacheSize) : nullptr),
      symbolCache_(ownedSymbolCache_.get()) {}

Symbolizer::Symbolizer(
    ElfCacheBase* cache,
    LocationInfoMode mode,
    SymbolCacheBase& symbolCache,
    std::string exePath)
    : cache_(cache ? cache : defaultElfCache()),
      mode_(mode),
      exePath_(std::move(exePath)),
      symbolCache_(&symbolCache) {}

Symbolizer::~Symbolizer() {}

size_t Symbolizer::symbolize(
    folly::Range<const uintptr_t*> addrs,
    folly::Range<SymbolizedFrame*> frames) {
  size_t addrCount = addrs.size();
  size_t frameCount = frames.size();
  if (addrCount > frameCount) {
    FOLLY_SAFE_DFATAL(
        "Not enough frames: addrCount: ",
        addrCount,
        " frameCount: ",
        frameCount);
    return 0;
  }
  size_t remaining = addrCount;

  auto const dbg = detail::get_r_debug();
  if (dbg == nullptr) {
    return 0;
  }
  if (dbg->r_version != 1) {
    return 0;
  }

  char selfPath[PATH_MAX + 8];
  ssize_t selfSize;
  if ((selfSize = readlink(exePath_.c_str(), selfPath, PATH_MAX + 1)) == -1) {
    // Something has gone terribly wrong.
    return 0;
  }
  selfPath[selfSize] = '\0';

  // If we call symbolize on the same range of frames twice, results are
  // slightly different. This is happening because we copy over the addresses
  // again but in the second pass we don't hit the code for adjusting the
  // address anymore. Therefore skipping copying over the addresses again if
  // frames are already filled.
  for (size_t i = 0; i < addrCount; i++) {
    if (frames[i].found) {
      continue;
    }
    frames[i].addr = addrs[i];
  }

  for (auto lmap = dbg->r_map; lmap != nullptr && remaining != 0;
       lmap = lmap->l_next) {
    // The empty string is used in place of the filename for the link_map
    // corresponding to the running executable.  Additionally, the `l_addr' is
    // 0 and the link_map appears to be first in the list---but none of this
    // behavior appears to be documented, so checking for the empty string is
    // as good as anything.
    auto const objPath = lmap->l_name[0] != '\0' ? lmap->l_name : selfPath;
    auto const elfFile = cache_->getFile(objPath);
    if (!elfFile) {
      continue;
    }

    for (size_t i = 0; i < addrCount && remaining != 0; ++i) {
      auto& frame = frames[i];
      if (frame.found) {
        continue;
      }

      auto const addr = frame.addr;
      // Room left at the end of `frames` to expand inline calls into.
      size_t const inlineCapacity = frameCount - addrCount;

      CachedSymbolizedFrames resolved;
      if (symbolCache_) {
        symbolCache_->find(addr, resolved);
      }

      if (!resolved.empty()) {
        if (resolved.size() - 1 > inlineCapacity) {
          // Not enough room for all the cached inline frames. Keep the
          // innermost ones that fit, and the non-inlined call at the end.
          resolved.erase(resolved.begin() + inlineCapacity, resolved.end() - 1);
        }
      } else {
        // Get the unrelocated, ELF-relative address by normalizing via the
        // address at which the object is loaded.
        auto const eaddr = static_cast<ElfAddr>(addr);
        auto const maddr = lmap->l_addr;
        auto const adjusted = eaddr < maddr ? ~ElfAddr(0) : eaddr - maddr;
        if (!containedInExecutableSegment(*elfFile, adjusted)) {
          continue; // Not in this object, retry against the next one.
        }

        size_t const maxInline = mode_ == LocationInfoMode::FULL_WITH_INLINE
            ? std::min<size_t>(kMaxInlineLocationInfoPerFrame, inlineCapacity)
            : 0;
        resolved =
            symbolizeAddress(cache_, elfFile, adjusted, mode_, maxInline);
        if (symbolCache_) {
          // Frames may already have been set here. It's fine to overwrite as
          // the value should be the same.
          symbolCache_->insert(addr, resolved);
        }
      }

      --remaining;
      size_t const numInlined = spliceFrames(frames, i, addrCount, resolved);
      addrCount += numInlined;
      i += numInlined; // Skip over the newly added inlined items.
    }
  }

  return addrCount;
}

FastStackTracePrinter::FastStackTracePrinter(
    std::unique_ptr<SymbolizePrinter> printer, size_t symbolCacheSize)
    : printer_(std::move(printer)),
      symbolizer_(defaultElfCache(), LocationInfoMode::FULL, symbolCacheSize) {}

FastStackTracePrinter::~FastStackTracePrinter() = default;

void FastStackTracePrinter::printStackTrace(bool symbolize) {
  SCOPE_EXIT {
    printer_->flush();
  };

  FrameArray<kMaxStackTraceDepth> addresses;

  if (!getStackTraceSafe(addresses)) {
    printer_->print("(error retrieving stack trace)\n");
  } else {
    symbolizeAndPrint(printer_, symbolizer_, addresses, symbolize);
  }

  addresses.frameCount = 0;
  if (!getAsyncStackTraceSafe(addresses) || addresses.frameCount == 0) {
    return;
  }
  printAsyncStackInfo([this](auto sp) { printer_->print(sp); });
  symbolizeAndPrint(printer_, symbolizer_, addresses, symbolize);
}

void FastStackTracePrinter::flush() {
  printer_->flush();
}

TwoStepFastStackTracePrinter::TwoStepFastStackTracePrinter(
    std::unique_ptr<SymbolizePrinter> printer, size_t symbolCacheSize)
    : printer_(std::move(printer)),
      symbolizer_(defaultElfCache(), LocationInfoMode::FULL, symbolCacheSize) {
  getStackTraceSafe(syncAddresses_);
  getAsyncStackTraceSafe(asyncAddresses_);
}

void TwoStepFastStackTracePrinter::printStackTrace(bool symbolize) {
  std::lock_guard lock(mutex_);
  SCOPE_EXIT {
    printer_->flush();
  };

  if (syncAddresses_.frameCount == 0) {
    printer_->print("(error retrieving stack trace)\n");
  } else {
    symbolizeAndPrint(printer_, symbolizer_, syncAddresses_, symbolize);
  }

  if (asyncAddresses_.frameCount == 0) {
    return;
  }
  printAsyncStackInfo([this](auto sp) { printer_->print(sp); });
  symbolizeAndPrint(printer_, symbolizer_, asyncAddresses_, symbolize);
}
#endif // FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

SafeStackTracePrinter::SafeStackTracePrinter(int fd)
    : fd_(fd),
      printer_(
          fd,
          SymbolizePrinter::COLOR_IF_TTY,
          size_t(64) << 10), // 64KiB
      addresses_(std::make_unique<FrameArray<kMaxStackTraceDepth>>()) {}

void SafeStackTracePrinter::flush() {
  printer_.flush();
  fsyncNoInt(fd_);
}

void SafeStackTracePrinter::printSymbolizedStackTrace() {
#if FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF
  // This function might run on an alternative stack allocated by
  // UnsafeSelfAllocateStackTracePrinter. Capturing a stack from
  // here is probably wrong.

  // Do our best to populate location info, process is going to terminate,
  // so performance isn't critical.
  SignalSafeElfCache elfCache_;
  Symbolizer symbolizer(&elfCache_, LocationInfoMode::FULL);
  symbolizer.symbolize(*addresses_);

  // Skip the top 2 frames captured by printStackTrace:
  // getStackTraceSafe
  // SafeStackTracePrinter::printStackTrace (captured stack)
  //
  // Leaving signalHandler on the stack for clarity, I think.
  printer_.println(*addresses_, 2);
#else
  printUnsymbolizedStackTrace();
#endif
}

void SafeStackTracePrinter::printUnsymbolizedStackTrace() {
  print("(safe mode, symbolizer not available)\n");
#if FOLLY_HAVE_BACKTRACE
  // `backtrace_symbols_fd` from execinfo.h is not explicitly
  // documented on either macOS or Linux to be async-signal-safe, but
  // the implementation in
  // https://opensource.apple.com/source/Libc/Libc-1353.60.8/ appears
  // safe.
  ::backtrace_symbols_fd(
      reinterpret_cast<void**>(addresses_->addresses),
      addresses_->frameCount,
      fd_);
#else
  AddressFormatter formatter;
  for (size_t i = 0; i < addresses_->frameCount; ++i) {
    print(formatter.format(addresses_->addresses[i]));
    print("\n");
  }
#endif
}

void SafeStackTracePrinter::printStackTrace(bool symbolize) {
  SCOPE_EXIT {
    flush();
  };

  // Skip the getStackTrace frame
  if (!getStackTraceSafe(*addresses_)) {
    print("(error retrieving stack trace)\n");
  } else if (symbolize) {
    printSymbolizedStackTrace();
  } else {
    printUnsymbolizedStackTrace();
  }

  addresses_->frameCount = 0;
  if (!getAsyncStackTraceSafe(*addresses_) || addresses_->frameCount == 0) {
    return;
  }
  printAsyncStackInfo([this](auto sp) { print(sp); });
  if (symbolize) {
    printSymbolizedStackTrace();
  } else {
    printUnsymbolizedStackTrace();
  }
}

#if FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF
namespace {
constexpr size_t kMaxStackTraceDepth = 100;

template <size_t N, typename StackTraceFunc>
std::string getStackTraceStrImpl(
    StackTraceFunc func, bool showFullInfo = false, size_t skip = 0) {
  FrameArray<N> addresses;

  if (!func(addresses)) {
    return "";
  } else {
    return detail::getStackTraceStr(
        addresses.addresses,
        addresses.frames,
        addresses.frameCount,
        showFullInfo,
        skip);
  }
}
} // namespace

namespace detail {
std::string getStackTraceStr(
    const uintptr_t* addresses,
    SymbolizedFrame* frames,
    size_t frameCount,
    bool showFullInfo,
    size_t skip) {
  if (frameCount == 0 || skip >= frameCount) {
    return {};
  }

  LocationInfoMode mode =
      showFullInfo ? LocationInfoMode::FULL : LocationInfoMode::FAST;
  Symbolizer symbolizer(/*cache=*/nullptr, mode, getSharedSymbolCache(mode));
  symbolizer.symbolize(addresses, frames, frameCount);

  StringSymbolizePrinter printer;
  printer.println(frames + skip, frameCount - skip);
  return printer.str();
}
} // namespace detail

std::string getStackTraceStr(bool showFullInfo, size_t skip) {
  return getStackTraceStrImpl<kMaxStackTraceDepth>(
      getStackTrace<kMaxStackTraceDepth>, showFullInfo, skip);
}

std::string getAsyncStackTraceStr(size_t skip) {
  return getStackTraceStrImpl<kMaxStackTraceDepth>(
      getAsyncStackTraceSafe<kMaxStackTraceDepth>, skip);
}

std::vector<std::string> getSuspendedStackTraces() {
  std::vector<std::string> stacks;
  sweepSuspendedLeafFrames([&](AsyncStackFrame* topFrame) {
    stacks.emplace_back(
        getStackTraceStrImpl<kMaxStackTraceDepth>([topFrame](auto& frameArray) {
          return detail::fixFrameArray(
              frameArray,
              getAsyncStackTraceFromInitialFrame(
                  topFrame, frameArray.addresses, kMaxStackTraceDepth));
        }));
  });
  return stacks;
}
#endif // FOLLY_HAVE_ELF && FOLLY_HAVE_DWARF

#if FOLLY_HAVE_SWAPCONTEXT

// Stack utilities used by UnsafeSelfAllocateStackTracePrinter
namespace {
// Size of mmap-allocated stack. Not to confuse with sigaltstack.
const size_t kMmapStackSize = 1 * 1024 * 1024;

constexpr bool kUseAllocatedStack = !kIsSanitizeThread;

template <typename Function>
Function resolve_libc_function(const char* name) {
  auto* symbol = detail::get_libc_symbol(name);
  return reinterpret_cast<Function>(symbol);
}

} // namespace

struct UnsafeSelfAllocateStackTracePrinter::AllocatedStack {
  AllocatedStack() = default;
  AllocatedStack(const AllocatedStack&) = delete;
  AllocatedStack& operator=(const AllocatedStack&) = delete;
  AllocatedStack(AllocatedStack&&) = delete;
  AllocatedStack& operator=(AllocatedStack&&) = delete;

  ~AllocatedStack() {
    if (mapping) {
      munmap(mapping, kMmapStackSize);
    }
  }

  char* mapping{nullptr};
  void* bottom{nullptr};
  size_t size{0};
};

struct UnsafeSelfAllocateStackTracePrinter::ContextFunctions {
  ContextFunctions()
      : getcontext(
            resolve_libc_function<decltype(&::getcontext)>("getcontext")),
        makecontext(
            resolve_libc_function<decltype(&::makecontext)>("makecontext")),
        swapcontext(
            resolve_libc_function<decltype(&::swapcontext)>("swapcontext")) {}

  explicit operator bool() const {
    return getcontext && makecontext && swapcontext;
  }

  decltype(&::getcontext) getcontext;
  decltype(&::makecontext) makecontext;
  decltype(&::swapcontext) swapcontext;
};

#if __has_attribute(disable_sanitizer_instrumentation)
#define FOLLY_DETAIL_SYMBOLIZER_RAW                  \
  __attribute__((                                    \
      noinline,                                      \
      no_sanitize("address", "undefined", "thread"), \
      disable_sanitizer_instrumentation))
#else
#define FOLLY_DETAIL_SYMBOLIZER_RAW \
  __attribute__((noinline, no_sanitize("address", "undefined", "thread")))
#endif

FOLLY_PUSH_WARNING

// On Apple platforms, some ucontext methods that are used here are deprecated.
#ifdef __APPLE__
FOLLY_GNU_DISABLE_WARNING("-Wdeprecated-declarations")
#endif

std::unique_ptr<UnsafeSelfAllocateStackTracePrinter::AllocatedStack>
UnsafeSelfAllocateStackTracePrinter::allocateStack() {
  const long pageSizeUnchecked = sysconf(_SC_PAGESIZE);
  if (pageSizeUnchecked <= 0) {
    return nullptr;
  }
  const auto pageSize = static_cast<size_t>(pageSizeUnchecked);
  if (pageSize * 2 >= kMmapStackSize) {
    return nullptr;
  }

  auto* mapping = static_cast<char*>(mmap(
      nullptr,
      kMmapStackSize,
      PROT_WRITE | PROT_READ,
      MAP_ANONYMOUS | MAP_PRIVATE,
      /* fd */ -1,
      /* offset */ 0));
  if (mapping == MAP_FAILED) {
    return nullptr;
  }
  auto rollback = makeGuard([&] { munmap(mapping, kMmapStackSize); });
  auto stack = std::make_unique<AllocatedStack>();
  stack->mapping = mapping;
  rollback.dismiss();

  const size_t upperBound = ((kMmapStackSize - 1) / pageSize) * pageSize;
  if (mprotect(mapping, pageSize, PROT_NONE) != 0 ||
      mprotect(mapping + upperBound, kMmapStackSize - upperBound, PROT_NONE) !=
          0) {
    return nullptr;
  }

  stack->bottom = mapping + pageSize;
  stack->size = upperBound - pageSize;
  return stack;
}

UnsafeSelfAllocateStackTracePrinter::UnsafeSelfAllocateStackTracePrinter(int fd)
    : SafeStackTracePrinter(fd),
      stack_(kUseAllocatedStack ? allocateStack() : nullptr),
      contextFunctions_(
          kUseAllocatedStack ? std::make_unique<ContextFunctions>() : nullptr) {
  // Initialize the pthread TLS key before a fatal signal can request an async
  // stack. No stack is captured when there is no active async root.
  (void)tryGetCurrentAsyncStackRoot();
}

UnsafeSelfAllocateStackTracePrinter::~UnsafeSelfAllocateStackTracePrinter() =
    default;

FOLLY_DETAIL_SYMBOLIZER_RAW void
UnsafeSelfAllocateStackTracePrinter::contextStart(
    UnsafeSelfAllocateStackTracePrinter* that) {
  void const* fromStack;
  size_t fromStackSize;
  sanitizer_finish_switch_fiber(nullptr, &fromStack, &fromStackSize);
  if (that) {
    that->SafeStackTracePrinter::printSymbolizedStackTrace();
  }
  sanitizer_start_switch_fiber(nullptr, fromStack, fromStackSize);
}

FOLLY_DETAIL_SYMBOLIZER_RAW bool
UnsafeSelfAllocateStackTracePrinter::runOnAllocatedStack() {
  if (!stack_ || !contextFunctions_ || !*contextFunctions_) {
    return false;
  }

  ucontext_t cur;
  ucontext_t alt;
  if (contextFunctions_->getcontext(&alt) != 0) {
    return false;
  }
  alt.uc_link = &cur;
  alt.uc_stack.ss_sp = stack_->bottom;
  alt.uc_stack.ss_size = stack_->size;
  alt.uc_stack.ss_flags = 0;

  contextFunctions_->makecontext(
      &alt,
      (void (*)())(void (*)(UnsafeSelfAllocateStackTracePrinter*))(
          &UnsafeSelfAllocateStackTracePrinter::contextStart),
      /* argc */ 1,
      /* arg */ this);
  void* currentFakestack;
  sanitizer_start_switch_fiber(
      &currentFakestack, alt.uc_stack.ss_sp, alt.uc_stack.ss_size);
  // NOTE: swapcontext is not async-signal-safe
  const bool switched = contextFunctions_->swapcontext(&cur, &alt) == 0;
  if (!switched) {
    contextStart(nullptr);
  }
  sanitizer_finish_switch_fiber(currentFakestack, nullptr, nullptr);
  return switched;
}

FOLLY_DETAIL_SYMBOLIZER_RAW void
UnsafeSelfAllocateStackTracePrinter::printSymbolizedStackTrace() {
  const bool printed = kUseAllocatedStack && runOnAllocatedStack();
  if (!printed) {
    printUnsymbolizedStackTrace();
  }
}

FOLLY_POP_WARNING

#undef FOLLY_DETAIL_SYMBOLIZER_RAW

#endif // FOLLY_HAVE_SWAPCONTEXT

} // namespace symbolizer
} // namespace folly
