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

#include <folly/system/ThreadName.h>

#include <atomic>
#include <type_traits>

#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/Traits.h>
#include <folly/portability/PThread.h>
#include <folly/portability/Windows.h>

#ifdef _WIN32
#include <strsafe.h> // @manual
#endif

// Android only, prctl is only used when pthread_setname_np
// and pthread_getname_np are not avilable.
#if defined(__linux__)
#define FOLLY_DETAIL_HAS_PRCTL_PR_SET_NAME 1
#else
#define FOLLY_DETAIL_HAS_PRCTL_PR_SET_NAME 0
#endif

#if FOLLY_DETAIL_HAS_PRCTL_PR_SET_NAME
#include <sys/prctl.h>
#endif

// This looks a bit weird, but it's necessary to avoid
// having an undefined compiler function called.
#if defined(__GLIBC__) && !defined(__APPLE__) && !defined(__ANDROID__)
// has pthread_setname_np(pthread_t, const char*) (2 params)
#define FOLLY_HAS_PTHREAD_SETNAME_NP_THREAD_NAME 1
// pthread_setname_np was introduced in Android NDK version 9
#elif defined(__ANDROID__) && __ANDROID_API__ >= 9
#define FOLLY_HAS_PTHREAD_SETNAME_NP_THREAD_NAME 1
#else
#define FOLLY_HAS_PTHREAD_SETNAME_NP_THREAD_NAME 0
#endif

#if defined(__APPLE__)
#define FOLLY_HAS_PTHREAD_SETNAME_NP_NAME 1
#else
#define FOLLY_HAS_PTHREAD_SETNAME_NP_NAME 0
#endif // defined(__APPLE__)

namespace folly {

namespace {

#if FOLLY_HAVE_PTHREAD && !defined(_WIN32)
pthread_t stdTidToPthreadId(std::thread::id tid) {
  static_assert(
      std::is_same<pthread_t, std::thread::native_handle_type>::value,
      "This assumes that the native handle type is pthread_t");
  static_assert(
      sizeof(std::thread::native_handle_type) == sizeof(std::thread::id),
      "This assumes std::thread::id is a thin wrapper around "
      "std::thread::native_handle_type, but that doesn't appear to be true.");
  // In most implementations, std::thread::id is a thin wrapper around
  // std::thread::native_handle_type, which means we can do unsafe things to
  // extract it.
  pthread_t id;
  std::memcpy(&id, &tid, sizeof(id));
  return id;
}
#endif

} // namespace

bool canSetCurrentThreadName() {
#if FOLLY_HAS_PTHREAD_SETNAME_NP_THREAD_NAME ||                                \
    FOLLY_HAS_PTHREAD_SETNAME_NP_NAME || FOLLY_DETAIL_HAS_PRCTL_PR_SET_NAME || \
    defined(_WIN32)
  return true;
#else
  return false;
#endif
}

bool canSetOtherThreadName() {
#if (FOLLY_HAS_PTHREAD_SETNAME_NP_THREAD_NAME) || defined(_WIN32)
  return true;
#else
  return false;
#endif
}

static constexpr size_t kMaxThreadNameLength = 16;

#if FOLLY_HAVE_PTHREAD
static Optional<std::string> getPThreadName(pthread_t pid) {
#if (                                           \
    FOLLY_HAS_PTHREAD_SETNAME_NP_THREAD_NAME || \
    FOLLY_HAS_PTHREAD_SETNAME_NP_NAME) &&       \
    !defined(__ANDROID__)
  // Android NDK does not yet support pthread_getname_np.
  std::array<char, kMaxThreadNameLength> buf;
  if (pthread_getname_np(pid, buf.data(), buf.size()) == 0) {
    return std::string(buf.data());
  }
#endif
  (void)pid;
  return none;
}
#endif

Optional<std::string> getThreadName(std::thread::id id) {
#if (                                           \
    FOLLY_HAS_PTHREAD_SETNAME_NP_THREAD_NAME || \
    FOLLY_HAS_PTHREAD_SETNAME_NP_NAME) &&       \
    !defined(__ANDROID__)
  // Android NDK does not yet support pthread_getname_np.
  if (id != std::thread::id()) {
    return getPThreadName(stdTidToPthreadId(id));
  }
#elif FOLLY_DETAIL_HAS_PRCTL_PR_SET_NAME
  std::array<char, kMaxThreadNameLength> buf;
  if (id == std::this_thread::get_id() &&
      prctl(PR_GET_NAME, buf.data(), 0L, 0L, 0L) == 0) {
    return std::string(buf.data());
  }
#endif
  (void)id;
  return none;
} // namespace folly

Optional<std::string> getCurrentThreadName() {
#if FOLLY_HAVE_PTHREAD
  return getPThreadName(pthread_self());
#else
  return getThreadName(std::this_thread::get_id());
#endif
}

namespace {
#ifdef _WIN32

typedef HRESULT(__stdcall* SetThreadDescriptionFn)(HANDLE, PCWSTR);

SetThreadDescriptionFn getSetThreadDescription() {
  // GetModuleHandle does not increment the module's reference count,
  // but kernelbase.dll won't be unloaded.
  auto proc = GetProcAddress(
      GetModuleHandleW(L"kernelbase.dll"), "SetThreadDescription");
  return reinterpret_cast<SetThreadDescriptionFn>(proc);
}

bool setThreadNameWindowsViaDescription(DWORD id, StringPiece name) noexcept {
  // This whole function is noexcept.

  static const auto setThreadDescription = getSetThreadDescription();
  if (!setThreadDescription) {
    return false;
  }

  HANDLE thread = id == GetCurrentThreadId()
      ? GetCurrentThread()
      : OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE, id);
  if (!thread) {
    return false;
  }

  SCOPE_EXIT {
    if (id != GetCurrentThreadId()) {
      CloseHandle(thread);
    }
  };

  // Limit thread name length so the UTF-16 output buffer can be fixed-length.
  // Pthreads limits thread names to 15, but that's pretty tight in practice. 64
  // should be plenty.
  constexpr size_t kMaximumThreadNameLength = 64;
  if (name.size() > kMaximumThreadNameLength) {
    name = name.subpiece(0, kMaximumThreadNameLength);
  }

  // For valid UTF-8, code points in [0, 0xFFFF] fit in one UTF-16 code unit.
  // Code points that require two UTF-16 code units are always encoded with four
  // UTF-8 bytes. Therefore, the fixed output buffer can be the maximum name
  // length in WCHARs, plus one for the terminating zero. That said, who knows
  // what MultiByteToWideChar does with invalid UTF-8 or if it attempts to
  // compose or decompose forms, so double-check the output anyway.
  WCHAR wname[kMaximumThreadNameLength + 1];
  int written = MultiByteToWideChar(
      CP_UTF8, 0, name.data(), name.size(), wname, kMaximumThreadNameLength);
  if (written <= 0 || static_cast<size_t>(written) >= std::size(wname)) {
    // Conversion failed or somehow it didn't fit.
    return false;
  }
  wname[written] = 0;

  if (FAILED(setThreadDescription(thread, wname))) {
    return false;
  }

  return true;
}
bool setThreadNameWindowsViaDebugger(DWORD id, StringPiece name) noexcept {
// http://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
#pragma pack(push, 8)
  struct THREADNAME_INFO {
    DWORD dwType; // Must be 0x1000
    LPCSTR szName; // Pointer to name (in user address space)
    DWORD dwThreadID; // Thread ID (-1 for caller thread)
    DWORD dwFlags; // Reserved for future use; must be zero
  };
  union TNIUnion {
    THREADNAME_INFO tni;
    ULONG_PTR upArray[4];
  };
#pragma pack(pop)

  static constexpr DWORD kMSVCException = 0x406D1388;

  char trimmed[kMaxThreadNameLength];
  if (STRSAFE_E_INVALID_PARAMETER ==
      StringCchCopyNA(trimmed, sizeof(trimmed), name.data(), name.size())) {
    return false;
  }
  // Intentionally ignore STRSAFE_E_INSUFFICIENT_BUFFER: the buffer now contains
  // a truncated, zero-terminated string, and that's desirable here.

  TNIUnion tniUnion = {{0x1000, trimmed, id, 0}};

  // SEH requires no use of C++ object destruction semantics in this stack
  // frame.
  __try {
    RaiseException(kMSVCException, 0, 4, tniUnion.upArray);
  } __except (
      GetExceptionCode() == kMSVCException
          ? EXCEPTION_CONTINUE_EXECUTION
          : EXCEPTION_EXECUTE_HANDLER) {
    // Swallow the exception when a debugger isn't attached.
  }
  return true;
}

bool setThreadNameWindows(std::thread::id tid, StringPiece name) {
  static_assert(
      sizeof(DWORD) == sizeof(std::thread::id),
      "This assumes std::thread::id is a thin wrapper around "
      "the Win32 thread id, but that doesn't appear to be true.");

  // std::thread::id is a thin wrapper around the Windows thread ID,
  // so just extract it.
  DWORD id;
  std::memcpy(&id, &tid, sizeof(id));

  // First, try the Windows 10 1607 SetThreadDescription call.
  if (setThreadNameWindowsViaDescription(id, name)) {
    return true;
  }

  // Otherwise, fall back to the older SEH approach, which is primarily
  // useful in debuggers.
  return setThreadNameWindowsViaDebugger(id, name);
}

#endif
} // namespace

bool setThreadName(std::thread::id tid, StringPiece name) {
#ifdef _WIN32
  return setThreadNameWindows(tid, name);
#else
  return setThreadName(stdTidToPthreadId(tid), name);
#endif
}

bool setThreadName(pthread_t pid, StringPiece name) {
#ifdef _WIN32
  static_assert(
      sizeof(unsigned int) == sizeof(std::thread::id),
      "This assumes std::thread::id is a thin wrapper around "
      "the thread id as an unsigned int, but that doesn't appear to be true.");

  // std::thread::id is a thin wrapper around an integral thread id,
  // so just stick the ID in.
  unsigned int tid = pthread_getw32threadid_np(pid);
  std::thread::id id;
  std::memcpy(&id, &tid, sizeof(id));
  return setThreadName(id, name);
#else
  name = name.subpiece(0, kMaxThreadNameLength - 1);
  char buf[kMaxThreadNameLength] = {};
  std::memcpy(buf, name.data(), name.size());
#if FOLLY_HAS_PTHREAD_SETNAME_NP_THREAD_NAME
  return 0 == pthread_setname_np(pid, buf);
#else
#if FOLLY_HAS_PTHREAD_SETNAME_NP_NAME
  // Since macOS 10.6 and iOS 3.2 it is possible for a thread
  // to set its own name using pthread, but
  // not that of some other thread.
  if (pthread_equal(pthread_self(), pid)) {
    return 0 == pthread_setname_np(buf);
  }
#elif FOLLY_DETAIL_HAS_PRCTL_PR_SET_NAME
  // for Android prctl is used instead of pthread_setname_np
  // if Android NDK version is older than API level 9.
  if (pthread_equal(pthread_self(), pid)) {
    return 0 == prctl(PR_SET_NAME, buf, 0L, 0L, 0L);
  }
#else
  (void)pid;
#endif
  return false;
#endif // !FOLLY_HAS_PTHREAD_SETNAME_NP_THREAD_NAME
#endif // !_WIN32
}

namespace {
std::atomic<CurrentThreadNameCallback> currentThreadNameCallback{nullptr};
} // namespace

bool setThreadName(StringPiece name) {
#if FOLLY_HAVE_PTHREAD
  const bool named = setThreadName(pthread_self(), name);
#else
  const bool named = setThreadName(std::this_thread::get_id(), name);
#endif
  if (const auto callback =
          currentThreadNameCallback.load(std::memory_order_acquire)) {
    callback(name);
  }
  return named;
}

CurrentThreadNameCallback setCurrentThreadNameCallback(
    CurrentThreadNameCallback callback) noexcept {
  return currentThreadNameCallback.exchange(
      callback, std::memory_order_acq_rel);
}
} // namespace folly
