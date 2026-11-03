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

#include <folly/lang/Exception.h>

#include <atomic>
#include <cassert>
#include <cstring>

#include <folly/lang/Align.h>
#include <folly/lang/New.h>

#if defined(__GLIBCXX__) || defined(_LIBCPP_VERSION)
#include <cxxabi.h>
#if !defined(__FreeBSD__)
#include <unwind.h>
#endif
#endif

#if defined(__GLIBCXX__) || defined(_LIBCPP_VERSION)
#if !defined(__FreeBSD__) // cxxabi.h already declares these

namespace __cxxabiv1 {

struct __cxa_eh_globals {
  void* caughtExceptions;
  unsigned int uncaughtExceptions;
};

#if defined(__GLIBCXX__)
extern "C" [[gnu::const]] __cxa_eh_globals* __cxa_get_globals() noexcept;
#else
extern "C" __cxa_eh_globals* __cxa_get_globals();
#endif

} // namespace __cxxabiv1

#endif
#endif

namespace folly {

namespace detail {

unsigned int* uncaught_exceptions_ptr() noexcept {
  assert(kIsGlibcxx || kIsLibcpp);
#if defined(__GLIBCXX__) || defined(_LIBCPP_VERSION)
  return &__cxxabiv1::__cxa_get_globals()->uncaughtExceptions;
#else
  return nullptr;
#endif
}

} // namespace detail

} // namespace folly

//  Accesses std::type_info and std::exception_ptr internals. Since these vary
//  by platform and library, import or copy the structure and function
//  signatures from each platform and library.
//
//  Support:
//    libstdc++ via libgcc libsupc++
//    libc++ via llvm libcxxabi
//    libc++ on freebsd via libcxxrt
//    win32 via msvc crt
//
//  Both libstdc++ and libc++ are based on cxxabi but they are not identical.
//
//  Reference: https://github.com/RedBeard0531/better_exception_ptr.

//  imports ---

#if defined(__GLIBCXX__)

//  https://github.com/gcc-mirror/gcc/blob/releases/gcc-10.2.0/libstdc++-v3/libsupc++/unwind-cxx.h

//  the definition of _Unwind_Ptr in libgcc/unwind-generic.h since unwind.h in
//  libunwind does not have this typedef
#if defined(__ia64__) && defined(__hpux__)
typedef unsigned _Unwind_Ptr __attribute__((__mode__(__word__)));
#else
using _Unwind_Ptr = unsigned long __attribute__((__mode__(__pointer__)));
#endif

namespace __cxxabiv1 {

//  libsupc++ itanium-abi __cxa_exception assumes that _Unwind_Exception is
//  maximally-aligned, and indeed libgcc itanium-abi _Unwind_Exception is so;
//  but libunwind itanium-abi _Unwind_Exception is not maximally-aligned
#ifdef __ARM_EABI_UNWINDER__
//  https://github.com/gcc-mirror/gcc/blob/releases/gcc-14.2.0/gcc/ginclude/unwind-arm-common.h#L119
static constexpr size_t __folly_unwind_exception_align = 8;
#else
//  https://github.com/gcc-mirror/gcc/blob/releases/gcc-14.2.0/libgcc/unwind-generic.h#L106
struct __folly_unwind_exception_align_t {
  [[gnu::aligned]] int data;
};
static constexpr size_t __folly_unwind_exception_align =
    alignof(__folly_unwind_exception_align_t);
#endif

static constexpr uint64_t __gxx_primary_exception_class =
    0x474E5543432B2B00; // GNCUC++\0
static constexpr uint64_t __gxx_dependent_exception_class =
    0x474E5543432B2B01; // GNCUC++\1

struct __cxa_exception {
  std::type_info* exceptionType;
  void(_GLIBCXX_CDTOR_CALLABI* exceptionDestructor)(void*);
  void (*unexpectedHandler)(); // no std::unexpected_handler in C++17
  std::terminate_handler terminateHandler;
  __cxa_exception* nextException;
  int handlerCount;
#ifdef __ARM_EABI_UNWINDER__
  __cxa_exception* nextPropagatingException;
  int propagationCount;
#else
  int handlerSwitchValue;
  const unsigned char* actionRecord;
  const unsigned char* languageSpecificData;
  _Unwind_Ptr catchTemp;
  void* adjustedPtr;
#endif
  alignas(__folly_unwind_exception_align) _Unwind_Exception unwindHeader;
};

struct __cxa_refcounted_exception {
  _Atomic_word referenceCount;
  __cxa_exception exc;
};

} // namespace __cxxabiv1

#endif // defined(__GLIBCXX__)

#if defined(_LIBCPP_VERSION) && !defined(__FreeBSD__)

//  https://github.com/llvm/llvm-project/blob/llvmorg-11.1.0/libcxx/include/exception
//  https://github.com/llvm/llvm-project/blob/llvmorg-11.1.0/libcxxabi/src/cxa_exception.h
//  https://github.com/llvm/llvm-project/blob/llvmorg-11.1.0/libcxxabi/src/cxa_exception.cpp
//  https://github.com/llvm/llvm-project/blob/llvmorg-11.1.0/libcxxabi/src/private_typeinfo.h

namespace std {

#if defined(_LIBCPP_FUNC_VIS) // llvm < 17
#define FOLLY_DETAIL_EXN_FUNC_VIS _LIBCPP_FUNC_VIS
#else // llvm >= 17
#define FOLLY_DETAIL_EXN_FUNC_VIS _LIBCPP_EXPORTED_FROM_ABI
#endif

typedef void (*unexpected_handler)();
FOLLY_DETAIL_EXN_FUNC_VIS unexpected_handler get_unexpected() _NOEXCEPT;

} // namespace std

namespace __cxxabiv1 {

//  the definition until llvm v10.0.0-rc2
struct __folly_cxa_exception_sans_reserve {
  using dtor_ret_t = std::conditional_t<folly::kIsArchWasm, void*, void>;

#if defined(__LP64__) || defined(_WIN64) || defined(_LIBCXXABI_ARM_EHABI)
  size_t referenceCount;
#endif
  std::type_info* exceptionType;
  dtor_ret_t (*exceptionDestructor)(void*);
  void (*unexpectedHandler)();
  std::terminate_handler terminateHandler;
  __folly_cxa_exception_sans_reserve* nextException;
  int handlerCount;
#if defined(_LIBCXXABI_ARM_EHABI)
  __folly_cxa_exception_sans_reserve* nextPropagatingException;
  int propagationCount;
#else
  int handlerSwitchValue;
  const unsigned char* actionRecord;
  const unsigned char* languageSpecificData;
  void* catchTemp;
  void* adjustedPtr;
#endif
#if !defined(__LP64__) && !defined(_WIN64) && !defined(_LIBCXXABI_ARM_EHABI)
  size_t referenceCount;
#endif
  _Unwind_Exception unwindHeader;
};

//  the definition since llvm v10.0.0-rc2
struct __folly_cxa_exception_with_reserve {
  using dtor_ret_t = std::conditional_t<folly::kIsArchWasm, void*, void>;

#if defined(__LP64__) || defined(_WIN64) || defined(_LIBCXXABI_ARM_EHABI)
  void* reserve;
  size_t referenceCount;
#endif
  std::type_info* exceptionType;
  dtor_ret_t (*exceptionDestructor)(void*);
  void (*unexpectedHandler)();
  std::terminate_handler terminateHandler;
  __folly_cxa_exception_with_reserve* nextException;
  int handlerCount;
#if defined(_LIBCXXABI_ARM_EHABI)
  __folly_cxa_exception_with_reserve* nextPropagatingException;
  int propagationCount;
#else
  int handlerSwitchValue;
  const unsigned char* actionRecord;
  const unsigned char* languageSpecificData;
  void* catchTemp;
  void* adjustedPtr;
#endif
#if !defined(__LP64__) && !defined(_WIN64) && !defined(_LIBCXXABI_ARM_EHABI)
  size_t referenceCount;
#endif
  _Unwind_Exception unwindHeader;
};

#if _LIBCPP_VERSION < 180000 || !_LIBCPP_AVAILABILITY_HAS_INIT_PRIMARY_EXCEPTION
static const uint64_t kOurExceptionClass = 0x434C4E47432B2B00; // CLNGC++\0
#endif

//  with pointer authentication, a virtual call authenticates the function
//  pointer against a discriminator derived from the mangled name of the method,
//  so the shim must have the same name and signature as the real one
class __shim_type_info : public std::type_info {
 public:
  virtual ~__shim_type_info() = 0;

  virtual void noop1() const = 0;
  virtual void noop2() const = 0;
  virtual bool can_catch(
      const __shim_type_info* thrown_type, void*& adjustedPtr) const = 0;
};
using __folly_shim_type_info = __shim_type_info;

} // namespace __cxxabiv1

namespace abi = __cxxabiv1;

#endif // defined(_LIBCPP_VERSION) && !defined(__FreeBSD__)

#if defined(__FreeBSD__)

//  https://github.com/freebsd/freebsd-src/blob/release/13.0.0/contrib/libcxxrt/cxxabi.h
//  https://github.com/freebsd/freebsd-src/blob/release/13.0.0/contrib/libcxxrt/typeinfo.h

namespace __cxxabiv1 {

#if _LIBCPP_VERSION < 180000 || !_LIBCPP_AVAILABILITY_HAS_INIT_PRIMARY_EXCEPTION
static const uint64_t kOurExceptionClass = 0x474E5543432B2B00; // GNUCC++\0
#endif

class __folly_shim_type_info {
 public:
  virtual ~__folly_shim_type_info() = 0;
  virtual bool __is_pointer_p() const = 0;
  virtual bool __is_function_p() const = 0;
  virtual bool __do_catch(
      std::type_info const* thrown_type,
      void** thrown_object,
      unsigned outer) const = 0;
  virtual bool __do_upcast(
      std::type_info const* target, void** thrown_object) const = 0;
};

extern "C" void* __cxa_allocate_exception(size_t thrown_size) noexcept;
extern "C" void __cxa_free_exception(void* thrown_exception) noexcept;

} // namespace __cxxabiv1

namespace abi = __cxxabiv1;

#endif // defined(__FreeBSD__)

#if defined(_WIN32)

#if defined(__clang__)
struct _s_ThrowInfo; // compiler intrinsic in msvc
typedef const struct _s_ThrowInfo _ThrowInfo;
#endif

#include <ehdata.h> // @manual

extern "C" _CRTIMP2 void* __cdecl __AdjustPointer(void*, PMD const&);

// clang for windows built-in is available under std::__GetExceptionInfo
#if !defined(__clang__)
template <class _E>
void* __GetExceptionInfo(_E); // builtin
#endif

#endif // defined(_WIN32)

//  implementations ---

namespace folly {

namespace detail {

namespace {

template <typename F>
class scope_guard_ {
 private:
  [[FOLLY_ATTR_NO_UNIQUE_ADDRESS]] F func_;
  bool live_{true};

 public:
  explicit scope_guard_(F func) noexcept : func_{func} {}
  ~scope_guard_() { live_ ? func_() : void(); }
  void dismiss() { live_ = false; }
};

} // namespace

std::atomic<int> exception_ptr_access_rt_cache_{0};

bool exception_ptr_access_rt_() noexcept {
  auto& cache = exception_ptr_access_rt_cache_;
  auto const result = exception_ptr_access_rt_v_();
  cache.store(result ? 1 : -1, std::memory_order_relaxed);
  return result;
}

std::type_info const* exception_ptr_exception_typeid(
    std::exception const& ex) noexcept {
  return type_info_of(ex);
}

#if defined(__GLIBCXX__)

bool exception_ptr_access_rt_v_() noexcept {
  static_assert(exception_ptr_access_ct, "mismatch");
  return true;
}

template <typename F>
static decltype(auto) cxxabi_with_cxa_exception(void* object, F f) {
  using cxa_exception = abi::__cxa_exception;
  auto exception = object ? static_cast<cxa_exception*>(object) - 1 : nullptr;
  return f(exception);
}

std::type_info const* exception_ptr_get_type_(
    std::exception_ptr const& ptr) noexcept {
  if (!ptr) {
    return nullptr;
  }
  auto object = reinterpret_cast<void* const&>(ptr);
  auto exception = static_cast<abi::__cxa_exception*>(object) - 1;
  return exception->exceptionType;
}

void* exception_ptr_get_object_(
    std::exception_ptr const& ptr,
    std::type_info const* const target) noexcept {
  if (!ptr) {
    return nullptr;
  }
  auto object = reinterpret_cast<void* const&>(ptr);
  auto type = exception_ptr_get_type_(ptr);
  return !target || target->__do_catch(type, &object, 1) ? object : nullptr;
}

std::size_t exception_ptr_use_count_(std::exception_ptr const& ptr) noexcept {
  if (!ptr) {
    return 0;
  }
  auto object = reinterpret_cast<void* const&>(ptr);
  auto exception = static_cast<abi::__cxa_refcounted_exception*>(object) - 1;
  return __atomic_load_n(&exception->referenceCount, __ATOMIC_RELAXED);
}

#endif // defined(__GLIBCXX__)

#if defined(_LIBCPP_VERSION) && !defined(__FreeBSD__)

bool exception_ptr_access_rt_v_() noexcept {
  static_assert(exception_ptr_access_ct || kIsAppleIOS, "mismatch");
  FOLLY_PUSH_WARNING
  FOLLY_CLANG_DISABLE_WARNING("-Wunsupported-availability-guard")
  return exception_ptr_access_ct //
#if __clang__
      || __builtin_available(iOS 12, *)
#endif
      ;
  FOLLY_POP_WARNING
}

static void* cxxabi_get_object(std::exception_ptr const& ptr) noexcept {
  return reinterpret_cast<void* const&>(ptr);
}

static bool cxxabi_cxa_exception_sans_reserve() noexcept {
  // detect and cache the layout of __cxa_exception in the loaded libc++abi
  //
  // for 32-bit arm-ehabi llvm ...
  //
  // before v5.0.1, _Unwind_Exception is alignas(4)
  // as of v5.0.1, _Unwind_Exception is alignas(8)
  // _Unwind_Exception is the last field in __cxa_exception
  //
  // before v10.0.0-rc2, __cxa_exception has 4b padding before the unwind field
  // as of v10.0.0-rc2, __cxa_exception moves the 4b padding to the start in a
  // field called reserve
  //
  // before 32-bit arm-ehabi llvm v10.0.0-rc2, the reserve field does not exist
  // in the struct explicitly but the refcount field is there instead due to
  // implicit padding
  //
  // before 32-bit arm-ehabi llvm v5.0.1, and before 64-bit llvm v10.0.0-rc2,
  // the reserve field is before the struct start and so is presumably before
  // the struct allocation and so must not be accessed
  //
  // __cxa_allocate_exception zero-fills the __cxa_exception before __cxa_throw
  // assigns fields so if, after incref, the refcount field is still zero, then
  // the runtime llvm is at least v5.0.1 and before v10.00-rc2 and then all the
  // fields except for the unwind field are shifted up by 4b
  //
  // prefer optimistic concurrency over pessimistic concurrency
  static std::atomic<int> cache{};
  if (auto value = cache.load(std::memory_order_relaxed)) {
    return value > 0;
  }
  auto object = abi::__cxa_allocate_exception(0);
  abi::__cxa_increment_exception_refcount(object);
  auto exception =
      static_cast<abi::__folly_cxa_exception_sans_reserve*>(object) - 1;
  auto result = exception->referenceCount == 1;
  assert(
      result ||
      (static_cast<abi::__folly_cxa_exception_with_reserve*>(object) - 1)
              ->referenceCount == 1);
  abi::__cxa_free_exception(object); // no need for decref
  cache.store(result ? 1 : -1, std::memory_order_relaxed);
  return result;
}

template <typename F>
static decltype(auto) cxxabi_with_cxa_exception(void* object, F f) {
  if (cxxabi_cxa_exception_sans_reserve()) {
    using cxa_exception = abi::__folly_cxa_exception_sans_reserve;
    auto exception = object ? static_cast<cxa_exception*>(object) - 1 : nullptr;
    return f(exception);
  } else {
    using cxa_exception = abi::__folly_cxa_exception_with_reserve;
    auto exception = object ? static_cast<cxa_exception*>(object) - 1 : nullptr;
    return f(exception);
  }
}

std::type_info const* exception_ptr_get_type_(
    std::exception_ptr const& ptr) noexcept {
  if (!ptr) {
    return nullptr;
  }
  auto object = cxxabi_get_object(ptr);
  return cxxabi_with_cxa_exception(object, [](auto exception) { //
    return exception->exceptionType;
  });
}

#if defined(__clang__)
__attribute__((no_sanitize("undefined", "cfi-vcall")))
#endif // defined(__clang__)
void* exception_ptr_get_object_(
    std::exception_ptr const& ptr,
    std::type_info const* const target) noexcept {
  if (!ptr) {
    return nullptr;
  }
  auto object = cxxabi_get_object(ptr);
  auto type = exception_ptr_get_type(ptr);
  auto starget = static_cast<abi::__folly_shim_type_info const*>(target);
  auto stype = static_cast<abi::__folly_shim_type_info const*>(type);
  return !target || starget->can_catch(stype, object) ? object : nullptr;
}

std::size_t exception_ptr_use_count_(std::exception_ptr const& ptr) noexcept {
  if (!ptr) {
    return 0;
  }
  auto object = cxxabi_get_object(ptr);
  return cxxabi_with_cxa_exception(object, [](auto exception) -> long {
    return __atomic_load_n(&exception->referenceCount, __ATOMIC_RELAXED);
  });
}

#endif // defined(_LIBCPP_VERSION) && !defined(__FreeBSD__)

#if defined(__FreeBSD__)

bool exception_ptr_access_rt_v_() noexcept {
  static_assert(exception_ptr_access_ct, "mismatch");
  return true;
}

template <typename F>
static decltype(auto) cxxabi_with_cxa_exception(void* object, F f) {
  using cxa_exception = abi::__cxa_exception;
  auto exception = object ? static_cast<cxa_exception*>(object) - 1 : nullptr;
  return f(exception);
}

std::type_info const* exception_ptr_get_type_(
    std::exception_ptr const& ptr) noexcept {
  if (!ptr) {
    return nullptr;
  }
  auto object = reinterpret_cast<void* const&>(ptr);
  auto exception = static_cast<abi::__cxa_exception*>(object) - 1;
  return exception->exceptionType;
}

void* exception_ptr_get_object_(
    std::exception_ptr const& ptr,
    std::type_info const* const target) noexcept {
  if (!ptr) {
    return nullptr;
  }
  auto object = reinterpret_cast<void* const&>(ptr);
  auto type = exception_ptr_get_type(ptr);
  auto starget = reinterpret_cast<abi::__folly_shim_type_info const*>(target);
  return !target || starget->__do_catch(type, &object, 1) ? object : nullptr;
}

std::size_t exception_ptr_use_count_(std::exception_ptr const& ptr) noexcept {
  if (!ptr) {
    return 0;
  }
  auto object = reinterpret_cast<void* const&>(ptr);
  auto exception = static_cast<abi::__cxa_exception*>(object) - 1;
  return __atomic_load_n(&exception->referenceCount, __ATOMIC_RELAXED);
}

#endif // defined(__FreeBSD__)

#if defined(_WIN32)

template <typename T>
static T* win32_decode_pointer(T* ptr) {
  return static_cast<T*>(
      DecodePointer(const_cast<void*>(static_cast<void const*>(ptr))));
}

static std::shared_ptr<EHExceptionRecord> const& win32_get_record_sptr(
    std::exception_ptr const& ptr) noexcept {
  return reinterpret_cast<std::shared_ptr<EHExceptionRecord> const&>(ptr);
}

static EHExceptionRecord* win32_get_record(
    std::exception_ptr const& ptr) noexcept {
  return win32_get_record_sptr(ptr).get();
}

static bool win32_eptr_throw_info_ptr_is_encoded() {
  // detect and cache whether this version of the microsoft c++ standard library
  // encodes the throw-info pointer in the std::exception_ptr internals
  //
  // earlier versions of std::exception_ptr did encode the throw-info pointer
  // but the most recent versions do not, as visible on github at
  // https://github.com/microsoft/STL
  //
  // prefer optimistic concurrency over pessimistic concurrency
  static std::atomic<int> cache{0}; // 0 uninit, -1 false, 1 true
  if (auto value = cache.load(std::memory_order_relaxed)) {
    return value > 0;
  }
  // detection is done by observing actual runtime behavior, using int as the
  // exception object type to save cost
#if defined(__clang__)
  auto info = std::__GetExceptionInfo(0);
#else
  auto info = __GetExceptionInfo(0);
#endif
  auto ptr = std::make_exception_ptr(0);
  auto rec = win32_get_record(ptr);
  int value = 0;
  if (info == rec->params.pThrowInfo) {
    value = -1;
  }
  if (info == win32_decode_pointer(rec->params.pThrowInfo)) {
    value = +1;
  }
  assert(value);
  // last writer wins for simplicity, assuming it to be impossible for multiple
  // writers to write different values
  cache.store(value, std::memory_order_relaxed);
  return value > 0;
}

static ThrowInfo* win32_throw_info(EHExceptionRecord* rec) {
  auto encoded = win32_eptr_throw_info_ptr_is_encoded();
  auto info = rec->params.pThrowInfo;
  return encoded ? win32_decode_pointer(info) : info;
}

static std::uintptr_t win32_throw_image_base(EHExceptionRecord* rec) {
#if _EH_RELATIVE_TYPEINFO
  return reinterpret_cast<std::uintptr_t>(rec->params.pThrowImageBase);
#else
  (void)rec;
  return 0;
#endif
}

bool exception_ptr_access_rt_v_() noexcept {
  static_assert(exception_ptr_access_ct, "mismatch");
  return true;
}

std::type_info const* exception_ptr_get_type_(
    std::exception_ptr const& ptr) noexcept {
  auto rec = win32_get_record(ptr);
  if (!rec) {
    return nullptr;
  }
  auto base = win32_throw_image_base(rec);
  auto info = win32_throw_info(rec);
  auto cta_ = base + info->pCatchableTypeArray;
  auto cta = reinterpret_cast<CatchableTypeArray*>(cta_);
  // assumption: the compiler emits the most-derived type first
  auto ct_ = base + cta->arrayOfCatchableTypes[0];
  auto ct = reinterpret_cast<CatchableType*>(ct_);
  auto td_ = base + ct->pType;
  auto td = reinterpret_cast<TypeDescriptor*>(td_);
  return reinterpret_cast<std::type_info*>(td);
}

void* exception_ptr_get_object_(
    std::exception_ptr const& ptr,
    std::type_info const* const target) noexcept {
  auto rec = win32_get_record(ptr);
  if (!rec) {
    return nullptr;
  }
  auto object = rec->params.pExceptionObject;
  if (!target) {
    return object;
  }
  auto base = win32_throw_image_base(rec);
  auto info = win32_throw_info(rec);
  auto cta_ = base + info->pCatchableTypeArray;
  auto cta = reinterpret_cast<CatchableTypeArray*>(cta_);
  for (int i = 0; i < cta->nCatchableTypes; i++) {
    auto ct_ = base + cta->arrayOfCatchableTypes[i];
    auto ct = reinterpret_cast<CatchableType*>(ct_);
    auto td_ = base + ct->pType;
    auto td = reinterpret_cast<TypeDescriptor*>(td_);
    if (*target == *reinterpret_cast<std::type_info*>(td)) {
      return __AdjustPointer(object, ct->thisDisplacement);
    }
  }
  return nullptr;
}

std::size_t exception_ptr_use_count_(std::exception_ptr const& ptr) noexcept {
  return win32_get_record_sptr(ptr).use_count();
}

#endif // defined(_WIN32)

} // namespace detail

namespace detail {

#if defined(__GLIBCXX__) || defined(_LIBCPP_VERSION)

[[gnu::const]] abi::__cxa_eh_globals& cxa_get_globals() noexcept {
#if !defined(__has_feature) || !FOLLY_HAS_FEATURE(cxx_thread_local)
  return *abi::__cxa_get_globals();
#elif defined(__XTENSA__)
  return *abi::__cxa_get_globals();
#else
  thread_local abi::__cxa_eh_globals* cache;
  return FOLLY_LIKELY(!!cache) ? *cache : *(cache = abi::__cxa_get_globals());
#endif
}

#endif

} // namespace detail

std::exception_ptr current_exception() noexcept {
#if defined(__APPLE__)
  return std::current_exception();
#elif defined(_CPPLIB_VER)
  return std::current_exception();
#elif defined(_LIBCPP_VERSION)
  return std::current_exception();
#else
  auto const& globals = detail::cxa_get_globals();
  auto const exception =
      static_cast<abi::__cxa_exception*>(globals.caughtExceptions);
  if (!exception) {
    return std::exception_ptr();
  }
  uint64_t exn_class{};
  std::memcpy( // exception_class may be uint64_t or char[8]
      &exn_class,
      &exception->unwindHeader.exception_class,
      sizeof(exn_class));
  switch (exn_class) {
    case abi::__gxx_primary_exception_class: {
      auto const object = static_cast<void const*>(exception + 1);
      assume(!!object);
      return std::exception_ptr(
          reinterpret_cast<std::exception_ptr const&>(object));
    }
    case abi::__gxx_dependent_exception_class: {
      auto const object = static_cast<void const*>(exception->exceptionType);
      assume(!!object);
      return std::exception_ptr(
          reinterpret_cast<std::exception_ptr const&>(object));
    }
    default:
      return std::exception_ptr();
  }
#endif
}

std::size_t exception_ptr_use_count(std::exception_ptr const& ptr) noexcept {
  return detail::exception_ptr_use_count_(ptr);
}

bool exception_ptr_unique(std::exception_ptr const& ptr) noexcept {
  return exception_ptr_use_count(ptr) == 1;
}

namespace detail {

template <typename Try>
std::exception_ptr catch_current_exception_(Try&& t) noexcept {
  return catch_exception(static_cast<Try&&>(t), current_exception);
}

template <typename Value>
static std::exception_ptr make_exception_ptr_from_rep_(Value value) noexcept {
  static_assert(sizeof(std::exception_ptr) == sizeof(Value));
  static_assert(alignof(std::exception_ptr) == alignof(Value));
  std::exception_ptr ptr;
  std::memcpy(static_cast<void*>(&ptr), &value, sizeof(value));
  return ptr;
}

#if defined(__GLIBCXX__)

std::exception_ptr make_exception_ptr_with_(
    make_exception_ptr_with_arg_ const& arg, void* func) noexcept {
  auto type = const_cast<std::type_info*>(arg.type);
  void* object = abi::__cxa_allocate_exception(arg.size);
  (void)abi::__cxa_init_primary_exception(object, type, arg.dtor);
  auto exception = static_cast<abi::__cxa_refcounted_exception*>(object) - 1;
  exception->referenceCount = 1;
  return catch_current_exception_([&] {
    scope_guard_ rollback{std::bind(abi::__cxa_free_exception, object)};
    arg.ctor(object, func);
    rollback.dismiss();
    return make_exception_ptr_from_rep_(object);
  });
}

#elif defined(_LIBCPP_VERSION)

[[maybe_unused]] static void exception_cleanup_(
    _Unwind_Reason_Code reason, _Unwind_Exception* uwexception) {
  if (reason == _URC_FOREIGN_EXCEPTION_CAUGHT) {
    auto handler = cxxabi_with_cxa_exception(uwexception + 1, [](auto exn) {
      return exn->terminateHandler;
    });
    folly::catch_exception(handler, folly::variadic_noop);
    std::abort();
  }
  abi::__cxa_decrement_exception_refcount(uwexception + 1);
}

std::exception_ptr make_exception_ptr_with_(
    make_exception_ptr_with_arg_ const& arg, void* func) noexcept {
  void* object = abi::__cxa_allocate_exception(arg.size);
  auto type = const_cast<std::type_info*>(arg.type);
#if _LIBCPP_VERSION >= 180000 && _LIBCPP_AVAILABILITY_HAS_INIT_PRIMARY_EXCEPTION
  (void)abi::__cxa_init_primary_exception(object, type, arg.dtor);
  cxxabi_with_cxa_exception(object, [&](auto exception) {
    exception->referenceCount = 1;
  });
#else
  cxxabi_with_cxa_exception(object, [&](auto exception) {
#if defined(__FreeBSD__)
    exception->unexpectedHandler = nullptr;
#else
    exception->unexpectedHandler = std::get_unexpected();
#endif
    exception->terminateHandler = std::get_terminate();
    exception->exceptionType = type;
    exception->exceptionDestructor = arg.dtor;
    exception->referenceCount = 1;
    std::memcpy( // exception_class may be uint64_t or char[8]
        &exception->unwindHeader.exception_class,
        &abi::kOurExceptionClass,
        sizeof(abi::kOurExceptionClass));
    exception->unwindHeader.exception_cleanup = exception_cleanup_;
  });
#endif
  return catch_current_exception_([&] {
    scope_guard_ rollback{std::bind(abi::__cxa_free_exception, object)};
    arg.ctor(object, func);
    rollback.dismiss();
    return make_exception_ptr_from_rep_(object);
  });
}

#else

std::exception_ptr make_exception_ptr_with_(
    make_exception_ptr_with_arg_ const&, void*) noexcept {
  return std::exception_ptr();
}

#endif

} // namespace detail

struct exception_shared_string::state {
  // refcount ops use relaxed order since the string is immutable: side-effects
  // need not be made visible to the destructor since there are none
  static constexpr auto relaxed = std::memory_order_relaxed;
  std::atomic<std::size_t> refs{0u};
  std::size_t const size{0u};
  static constexpr std::size_t object_size(std::size_t const len) noexcept {
    // combined allocation, and size must be a multiple of alignment
    return align_ceil(sizeof(state) + len + 1u, alignof(state));
  }
  static state* make(char const* const str, std::size_t const len) {
    constexpr auto align = std::align_val_t{alignof(state)};
    assert(len == std::strlen(str));
    auto addr = operator_new(object_size(len), align);
    return new (addr) state(str, len);
  }
  static state* make(std::size_t const len, format_sig_& ffun, void* fobj) {
    constexpr auto align = std::align_val_t{alignof(state)};
    auto addr = operator_new(object_size(len), align);
    return new (addr) state(len, ffun, fobj);
  }
  state(char const* const str, std::size_t const len) noexcept : size{len} {
    std::memcpy(static_cast<void*>(this + 1u), str, len + 1u);
  }
  state(std::size_t const len, format_sig_& ffun, void* fobj) : size{len} {
    auto const buf = static_cast<char*>(static_cast<void*>(this + 1u));
    ffun(fobj, buf, len);
    buf[len] = 0;
  }
  char const* what() const noexcept {
    return static_cast<char const*>(static_cast<void const*>(this + 1u));
  }
  static void copy(state& self) noexcept { self.refs.fetch_add(1u, relaxed); }
  static void ruin(state& self) noexcept {
    constexpr auto align = std::align_val_t{alignof(state)};
    if (!self.refs.load(relaxed) || !self.refs.fetch_sub(1u, relaxed)) {
      operator_delete(&self, object_size(self.size), align);
    }
  }
  static void copy(state* self) noexcept { !self ? void() : copy(*self); }
  static void ruin(state* self) noexcept { !self ? void() : ruin(*self); }
};

char const* exception_shared_string::from_state(state const* self) noexcept {
  return !self ? nullptr : reinterpret_cast<char const*>(self + 1u);
}
auto exception_shared_string::to_state(const tagged_what_t& w) noexcept
    -> state* {
  if (w.is_literal()) {
    return nullptr;
  }
  return reinterpret_cast<state*>(const_cast<char*>(w.what())) - 1u;
}

exception_shared_string::exception_shared_string(
    std::size_t const len, format_sig_& ffun, void* const fobj)
    : tagged_what_{vtag<false>, from_state(state::make(len, ffun, fobj))} {}

exception_shared_string::exception_shared_string(
    char const* const str, std::size_t const len)
    : tagged_what_{vtag<false>, from_state(state::make(str, len))} {}

exception_shared_string::exception_shared_string(
    exception_shared_string const& that) noexcept
    : tagged_what_{
          (state::copy(to_state(that.tagged_what_)), that.tagged_what_)} {}

exception_shared_string& exception_shared_string::operator=(
    exception_shared_string const& that) noexcept {
  if (this != &that) {
    ruin_state();
    state::copy(to_state(that.tagged_what_));
    tagged_what_ = that.tagged_what_;
  }
  return *this;
}

#if !defined(__NVCC__)

exception_shared_string& exception_shared_string::operator=(
    exception_shared_string&& that) noexcept {
  if (this != &that) {
    ruin_state();
    tagged_what_ = that.tagged_what_;
    that.tagged_what_ =
        tagged_what_t{vtag<true>, ""}; // safe-to-read moved-out state
  }
  return *this;
}

#endif

void exception_shared_string::ruin_state() noexcept {
  state::ruin(to_state(tagged_what_));
}

} // namespace folly
