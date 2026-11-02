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

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <DbgHelp.h> // @manual

#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <string>

#include <folly/Indestructible.h>
#include <folly/Range.h>
#include <folly/debugging/symbolizer/SignalHandler.h>
#include <folly/lang/ToAscii.h>

namespace folly {
namespace symbolizer {
namespace detail {

uintptr_t currentFatalSignalThreadId() {
  return static_cast<uintptr_t>(::GetCurrentThreadId());
}

} // namespace detail

namespace {

constexpr size_t kMaxFrames = 64;
constexpr size_t kMaxSymbolNameChars = 1024;

void writeStderrW(const wchar_t* w, int wlen);

void writeStderr(StringPiece sp) {
  DWORD written = 0;
  ::WriteFile(
      ::GetStdHandle(STD_ERROR_HANDLE),
      sp.data(),
      static_cast<DWORD>(sp.size()),
      &written,
      nullptr);
}

void writeStderrHex(uint64_t v) {
  char buf[2 + to_ascii_size_max<16, uint64_t>];
  buf[0] = '0';
  buf[1] = 'x';
  size_t n = to_ascii_lower<16>(buf + 2, buf + sizeof(buf), v);
  writeStderr(StringPiece(buf, n + 2));
}

void writeStderrDec(uint64_t v) {
  char buf[to_ascii_size_max_decimal<uint64_t>];
  size_t n = to_ascii_decimal(buf, v);
  writeStderr(StringPiece(buf, n));
}

const char* exceptionCodeName(DWORD code) {
  switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
      return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_IN_PAGE_ERROR:
      return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_STACK_OVERFLOW:
      return "EXCEPTION_STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
      return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:
      return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
      return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INVALID_OPERATION:
      return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
      return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_BREAKPOINT:
      return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_PRIV_INSTRUCTION:
      return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
      return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION:
      return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    default:
      return "UNKNOWN_EXCEPTION";
  }
}

void writeAccessViolationDetail(EXCEPTION_RECORD* rec) {
  if (rec->NumberParameters < 2) {
    return;
  }
  const auto accessType = rec->ExceptionInformation[0];
  const auto faultAddr = rec->ExceptionInformation[1];
  StringPiece op;
  switch (accessType) {
    case 0:
      op = "read";
      break;
    case 1:
      op = "write";
      break;
    case 8:
      op = "DEP execute";
      break;
    default:
      op = "unknown access";
      break;
  }
  writeStderr("  ");
  writeStderr(op);
  writeStderr(" at ");
  writeStderrHex(static_cast<uint64_t>(faultAddr));
  writeStderr("\n");
}

void writeFaultingModule(void* address) {
  HMODULE module = nullptr;
  if (!::GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(address),
          &module)) {
    return;
  }
  wchar_t path[1024];
  const DWORD length = ::GetModuleFileNameW(module, path, 1024);
  if (length > 0 && length < 1024) {
    writeStderr(" in ");
    writeStderrW(path, static_cast<int>(length));
  }
  writeStderr(" module offset ");
  writeStderrHex(
      reinterpret_cast<uint64_t>(address) - reinterpret_cast<uint64_t>(module));
}

void writeExceptionHeader(EXCEPTION_RECORD* rec) {
  writeStderr("\n*** Fatal exception ");
  writeStderrHex(rec->ExceptionCode);
  writeStderr(" (");
  writeStderr(exceptionCodeName(rec->ExceptionCode));
  writeStderr(")");
  writeStderr(" received by PID ");
  writeStderrDec(::GetCurrentProcessId());
  writeStderr(", stack trace: ***\n");
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
      rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) {
    writeAccessViolationDetail(rec);
  }
  writeStderr("  faulting instruction at ");
  writeStderrHex(reinterpret_cast<uint64_t>(rec->ExceptionAddress));
  if (rec->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
    writeFaultingModule(rec->ExceptionAddress);
  }
  writeStderr("\n");
}

void writeRawFrames(void* const* frames, WORD count) {
  for (WORD i = 0; i < count; ++i) {
    writeStderr("#");
    writeStderrDec(i);
    writeStderr(" ");
    writeStderrHex(reinterpret_cast<uint64_t>(frames[i]));
    writeStderr("\n");
  }
}

// Convert a UTF-16 string from a DbgHelp output (wchar_t*) to UTF-8 and
// emit it to stderr. WriteFile takes bytes; raw UTF-16 garbles non-ASCII
// names. Worst-case 4 UTF-8 bytes per wchar.
//
// Pass wlen == -1 for null-terminated input (WideCharToMultiByte's
// convention); pass an explicit length when DbgHelp gives one (e.g.
// SYMBOL_INFOW::NameLen).
void writeStderrW(const wchar_t* w, int wlen) {
  if (!w || wlen == 0) {
    return;
  }
  char buf[4096];
  int n = ::WideCharToMultiByte(
      CP_UTF8, 0, w, wlen, buf, sizeof(buf), nullptr, nullptr);
  if (n > 0) {
    writeStderr(StringPiece(buf, n));
  }
}

// Use POSIX path-separator for consistency.
void writeStderrWPath(const wchar_t* w) {
  if (!w) {
    return;
  }
  char buf[4096];
  int n = ::WideCharToMultiByte(
      CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
  if (n <= 0) {
    return;
  }
  // n includes the null terminator for the -1 form; drop it before write.
  int len = n - 1;
  for (int i = 0; i < len; ++i) {
    if (buf[i] == '\\') {
      buf[i] = '/';
    }
  }
  writeStderr(StringPiece(buf, len));
}

// Directory of the running executable, used as the DbgHelp symbol search path
// ("" on failure). Dynamic wstring because exe paths can exceed MAX_PATH.
std::wstring executableDir() {
  std::wstring path;
  DWORD size = 4096;
  DWORD length = 0;
  DWORD lastError = ERROR_INSUFFICIENT_BUFFER;
  while (lastError == ERROR_INSUFFICIENT_BUFFER) {
    path.resize(size);
    ::SetLastError(0);
    length = ::GetModuleFileNameW(nullptr, path.data(), size);
    lastError = ::GetLastError();
    if (length == 0 || lastError != ERROR_INSUFFICIENT_BUFFER) {
      break;
    }
    size *= 2;
  }

  if (length == 0) {
    path.clear();
  } else {
    path.resize(length);
    if (auto slash = path.find_last_of(L'\\'); slash != std::wstring::npos) {
      path.resize(slash);
    }
  }
  return path;
}

// Resolved at static init and never freed, so the crash path never allocates.
const folly::Indestructible<std::wstring> gSymbolSearchPath{executableDir()};

// Guards the once-only SymInitializeW. Plain bool: only the thread that claimed
// the fatal-signal gate reaches here, so access is single-threaded.
bool gDbgHelpInitialized = false;

// Deferred to the first fault: SymInitializeW(fInvadeProcess=TRUE) reads the
// PEB environment and walks loaded modules, which faults during static init
// under some test runners. By the first fault the process is fully built. Must
// run once: SymInitialize must not be called twice without SymCleanup.
void ensureDbgHelpInitialized() {
  if (gDbgHelpInitialized) {
    return;
  }
  gDbgHelpInitialized = true;
  ::SymSetOptions(
      SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS |
      SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  const wchar_t* searchPath =
      gSymbolSearchPath->empty() ? nullptr : gSymbolSearchPath->c_str();
  ::SymInitializeW(::GetCurrentProcess(), searchPath, TRUE);
}

// Single-thread entry is guaranteed by the detail::tryClaimFatalSignalThread()
// gate in fatalExceptionFilter, so no lock is needed here — matches
// folly's POSIX path, which also avoids mutex acquisition during signal
// handling because std::mutex isn't async-signal-safe.
//
// Uses the W (UTF-16) variants of Sym* throughout for proper Unicode
// symbol and path resolution.
void walkAndSymbolize(PCONTEXT context) {
  ensureDbgHelpInitialized();

  CONTEXT ctx = *context;
  HANDLE proc = ::GetCurrentProcess();
  HANDLE thread = ::GetCurrentThread();

  DWORD machineType;
  STACKFRAME64 frame{};
#if defined(_M_X64)
  machineType = IMAGE_FILE_MACHINE_AMD64;
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx.Rsp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_ARM64)
  machineType = IMAGE_FILE_MACHINE_ARM64;
  frame.AddrPC.Offset = ctx.Pc;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx.Fp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx.Sp;
  frame.AddrStack.Mode = AddrModeFlat;
#else
  return;
#endif

  // SYMBOL_INFOW is variable-length: trailing Name[1] is implicitly
  // extended by surplus bytes in the buffer; (kMaxSymbolNameChars + 1)
  // includes the null terminator. alignas matches the struct's natural
  // alignment.
  alignas(SYMBOL_INFOW) std::byte symBuf
      [sizeof(SYMBOL_INFOW) + (kMaxSymbolNameChars + 1) * sizeof(wchar_t)];
  auto* sym = reinterpret_cast<SYMBOL_INFOW*>(symBuf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFOW);
  sym->MaxNameLen = kMaxSymbolNameChars;

  IMAGEHLP_LINEW64 line{};
  line.SizeOfStruct = sizeof(line);

  for (size_t i = 0; i < kMaxFrames; ++i) {
    if (!::StackWalk64(
            machineType,
            proc,
            thread,
            &frame,
            &ctx,
            nullptr,
            ::SymFunctionTableAccess64,
            ::SymGetModuleBase64,
            nullptr)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) {
      break;
    }

    writeStderr("#");
    writeStderrDec(i);
    writeStderr(" ");
    writeStderrHex(frame.AddrPC.Offset);
    writeStderr(" ");

    DWORD64 dispSym = 0;
    DWORD dispLine = 0;
    if (::SymFromAddrW(proc, frame.AddrPC.Offset, &dispSym, sym)) {
      writeStderrW(sym->Name, static_cast<int>(sym->NameLen));
      if (::SymGetLineFromAddrW64(
              proc, frame.AddrPC.Offset, &dispLine, &line)) {
        writeStderr(" at ");
        writeStderrWPath(line.FileName);
        writeStderr(":");
        writeStderrDec(line.LineNumber);
      }
    } else {
      writeStderr("??");
    }
    writeStderr("\n");
  }
}

LONG WINAPI fatalExceptionFilter(PEXCEPTION_POINTERS p) {
  // First-chance debugger notifications (DBG_*) live below this
  // threshold and aren't fatal.
  constexpr DWORD kFirstFatalExceptionCode = 0x80000000;
  constexpr DWORD kMsvcCppExceptionCode = 0xE06D7363;
  // CloseHandle() on a handle marked protected; known folly socket-close
  // noise, not fatal.
  constexpr DWORD kStatusHandleNotClosable = 0xC0000235;
  // RPC_E_WRONG_THREAD — WinRT HWND-capture first-call noise.
  constexpr DWORD kRpcEWrongThread = 0x8001010D;

  const DWORD code = p->ExceptionRecord->ExceptionCode;
  if (code < kFirstFatalExceptionCode || code == kMsvcCppExceptionCode ||
      code == kStatusHandleNotClosable || code == kRpcEWrongThread) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // Single-thread gate, shared with the POSIX and terminate handlers.
  const uintptr_t threadId = detail::currentFatalSignalThreadId();
  const uintptr_t holder = detail::tryClaimFatalSignalThread(threadId);
  if (holder != 0) {
    if (holder == threadId) {
      // Recursive fault during symbolization: dump raw addrs only.
      void* frames[kMaxFrames];
      WORD n = ::CaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
      writeRawFrames(frames, n);
    }
    return EXCEPTION_CONTINUE_SEARCH;
  }

  detail::setFatalSignalReceived(true);

  writeExceptionHeader(p->ExceptionRecord);

  if (code == EXCEPTION_STACK_OVERFLOW) {
    // Faulting thread has no stack to symbolize on; defer to the OS
    // default handler (e.g. WER) for a minidump.
    writeStderr("  (stack exhausted; deferring to default handler)\n");
  } else {
    walkAndSymbolize(p->ContextRecord);
  }

  detail::invokeFatalSignalCallbacks();

  // Release the gate (as the POSIX path does) so a first-chance exception
  // caught downstream cannot permanently disable the handler for a later crash.
  detail::releaseFatalSignalThread();

  return EXCEPTION_CONTINUE_SEARCH;
}

// std::terminate is reached on paths the VEH never sees: a throw out of
// a noexcept function, a coroutine promise that calls std::terminate, a
// double-fault during exception unwinding, etc. See
// https://devblogs.microsoft.com/oldnewthing/20250711-00/ for the full
// rundown. Install a terminate_handler so those still produce a usable
// stderr backtrace.
std::terminate_handler gPreviousTerminate = nullptr;

[[noreturn]] void terminateHandler() {
  detail::setFatalSignalReceived(true);

  writeStderr("\n*** std::terminate called");
  if (std::exception_ptr exc = std::current_exception()) {
    try {
      std::rethrow_exception(exc);
    } catch (const std::exception& e) {
      writeStderr(" (");
      writeStderr(e.what());
      writeStderr(")");
    } catch (...) {
      writeStderr(" (non-std exception)");
    }
  }
  writeStderr(", stack trace: ***\n");

  // Serialize with the VEH path (DbgHelp is single-threaded): if another thread
  // is already symbolizing, skip our walk.
  const uintptr_t threadId = detail::currentFatalSignalThreadId();
  if (detail::tryClaimFatalSignalThread(threadId) == 0) {
    CONTEXT ctx{};
    ::RtlCaptureContext(&ctx);
    walkAndSymbolize(&ctx);
  }

  detail::invokeFatalSignalCallbacks();

  if (gPreviousTerminate) {
    gPreviousTerminate();
  }
  std::abort();
}

} // namespace

namespace detail {

void installFatalSignalHandlerWindows() {
  // VEH at priority 1 (CALL_FIRST): symbolize before any frame-based __except
  // (e.g. gtest's death-test handler) claims the exception, then continue the
  // search so other handlers (crashpad, WER) run. DbgHelp init is deferred.
  ::AddVectoredExceptionHandler(1, &fatalExceptionFilter);
  gPreviousTerminate = std::set_terminate(&terminateHandler);
}

} // namespace detail
} // namespace symbolizer
} // namespace folly

#endif // _WIN32
