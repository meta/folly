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

#include <folly/Singleton.h>

#ifndef _WIN32
#include <dlfcn.h>
#ifndef __wasi__ // wasi has no signals; the shutdown timer needs librt.
#include <signal.h>
#endif
#include <time.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <string>
#include <fmt/chrono.h>
#include <fmt/format.h>

#include <folly/Demangle.h>
#include <folly/ScopeGuard.h>
#include <folly/debugging/symbolizer/Symbolizer.h>
#include <folly/lang/SafeAssert.h>
#include <folly/portability/Config.h>
#include <folly/portability/FmtCompile.h>
#include <folly/system/AtFork.h>
// Before registrationComplete() we cannot assume that glog has been
// initialized, so we need to use RAW_LOG for any message that may be logged
// before that.
#include <glog/raw_logging.h>

// A statically linked wasi program has no dlsym.
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__ANDROID__) && \
    !defined(__wasi__)
#define FOLLY_SINGLETON_HAVE_DLSYM 1
#else
#define FOLLY_SINGLETON_HAVE_DLSYM 0
#endif

namespace folly {

#if FOLLY_SINGLETON_HAVE_DLSYM
namespace detail {
static void singleton_hs_init_weak(int* argc, char** argv[])
    __attribute__((__weakref__("hs_init")));
} // namespace detail
#endif

SingletonVault::Type SingletonVault::defaultVaultType() {
#if FOLLY_SINGLETON_HAVE_DLSYM
  bool isPython = dlsym(RTLD_DEFAULT, "Py_Main");
  bool isHaskell =
      detail::singleton_hs_init_weak || dlsym(RTLD_DEFAULT, "hs_init");
  bool isJVM = dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs");
  bool isD = dlsym(RTLD_DEFAULT, "_d_run_main");
  bool isCgo = dlsym(RTLD_DEFAULT, "_cgo_topofstack") ||
      dlsym(RTLD_DEFAULT, "_cgo_panic");

  return isPython || isHaskell || isJVM || isD || isCgo
      ? Type::Relaxed
      : Type::Strict;
#else
  return Type::Relaxed;
#endif
}

namespace detail {

struct SingletonVaultRegistrationCompleteHookState {
  std::mutex mutex;
  std::vector<SingletonVaultRegistrationCompleteHook> hooks;
  bool defaultVaultComplete{false};
};

namespace {

SingletonVaultRegistrationCompleteHookState& registrationCompleteHookState() {
  return createGlobal<SingletonVaultRegistrationCompleteHookState, void>();
}

void runRegistrationCompleteHook(
    SingletonVaultRegistrationCompleteHook hook) noexcept {
  try {
    hook();
  } catch (const std::exception& e) {
    RAW_LOG(
        WARNING,
        "SingletonVault registration-complete hook threw: %s",
        e.what());
  } catch (...) {
    RAW_LOG(WARNING, "SingletonVault registration-complete hook threw");
  }
}

void invokeSingletonVaultRegistrationCompleteHooks() noexcept {
  auto& state = registrationCompleteHookState();
  size_t count = 0;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.defaultVaultComplete) {
      return;
    }
    state.defaultVaultComplete = true;
    count = state.hooks.size();
  }
  for (size_t i = 0; i < count; ++i) {
    SingletonVaultRegistrationCompleteHook hook = nullptr;
    {
      std::lock_guard<std::mutex> lock(state.mutex);
      hook = state.hooks[i];
    }
    runRegistrationCompleteHook(hook);
  }
}

} // namespace

bool addSingletonVaultRegistrationCompleteHook(
    SingletonVaultRegistrationCompleteHook hook) noexcept {
  if (hook == nullptr) {
    return false;
  }
  auto& state = registrationCompleteHookState();
  bool run = false;
  {
    std::lock_guard<std::mutex> lock(state.mutex);
    if (std::find(state.hooks.begin(), state.hooks.end(), hook) !=
        state.hooks.end()) {
      return true;
    }
    try {
      state.hooks.push_back(hook);
    } catch (...) {
      RAW_LOG(
          WARNING,
          "SingletonVault registration-complete hook could not be registered");
      return false;
    }
    run = state.defaultVaultComplete;
  }
  if (run) {
    runRegistrationCompleteHook(hook);
  }
  return true;
}

std::string TypeDescriptor::name() const {
  auto ret = demangle(ti_.name());
  if (tag_ti_ != std::type_index(typeid(DefaultTag))) {
    ret += "/";
    ret += demangle(tag_ti_.name());
  }
  return ret.toStdString();
}

[[noreturn]] void singletonWarnDoubleRegistrationAndAbort(
    const TypeDescriptor& type) {
  FOLLY_SAFE_FATAL( // May happen before registrationComplete().
      fmt::format(
          fmt::runtime( //
              "Double registration of singletons of the same "
              "underlying type; check for multiple definitions "
              "of type folly::Singleton<{}>"),
          type.name())
          .c_str());
}

[[noreturn]] void singletonWarnLeakyDoubleRegistrationAndAbort(
    const TypeDescriptor& type) {
  FOLLY_SAFE_FATAL( // May happen before registrationComplete().
      fmt::format(
          fmt::runtime( //
              "Double registration of singletons of the same "
              "underlying type; check for multiple definitions "
              "of type folly::LeakySingleton<{}>"),
          type.name())
          .c_str());
}

[[noreturn]] void singletonWarnLeakyInstantiatingNotRegisteredAndAbort(
    const TypeDescriptor& type) {
  FOLLY_SAFE_FATAL( // May happen before registrationComplete().
      "Creating instance for unregistered singleton: ",
      type.name().c_str());
}

[[noreturn]] void singletonWarnRegisterMockEarlyAndAbort(
    const TypeDescriptor& type) {
  FOLLY_SAFE_FATAL( // May happen before registrationComplete().
      "Registering mock before singleton was registered: ",
      type.name().c_str());
}

void singletonWarnDestroyInstanceLeak(
    const TypeDescriptor& type, const void* ptr) {
  LOG(ERROR)
      << "Singleton of type " << type.name() << " has a "
      << "living reference at destroyInstances time; beware! Raw "
      << "pointer is " << ptr << ". It is very likely "
      << "that some other singleton is holding a shared_ptr to it. "
      << "This singleton will be leaked (even if a shared_ptr to it "
      << "is eventually released)."
      << "Make sure dependencies between these singletons are "
      << "properly defined.";
}

[[noreturn]] void singletonWarnCreateCircularDependencyAndAbort(
    const TypeDescriptor& type) {
  FOLLY_SAFE_FATAL("circular singleton dependency: ", type.name().c_str());
}

[[noreturn]] void singletonWarnCreateUnregisteredAndAbort(
    const TypeDescriptor& type) {
  FOLLY_SAFE_FATAL( // May happen before registrationComplete().
      "Creating instance for unregistered singleton: ",
      type.name().c_str());
}

[[noreturn]] void singletonWarnCreateBeforeRegistrationCompleteAndAbort(
    const TypeDescriptor& type) {
  FOLLY_SAFE_FATAL( // May happen before registrationComplete().
      fmt::format(
          fmt::runtime( //
              "Singleton {} requested before "
              "registrationComplete() call.\n"
              "This usually means that either main() never called "
              "folly::init, or singleton was requested before main() "
              "(which is not allowed)"),
          type.name())
          .c_str());
}

void singletonPrintDestructionStackTrace(const TypeDescriptor& type) {
  auto trace = symbolizer::getStackTraceStr();
  LOG(ERROR) << "Singleton " << type.name() << " was released.\n"
             << "Stacktrace:\n"
             << (!trace.empty() ? trace : "(not available)");
}

[[noreturn]] void singletonThrowNullCreator(const std::type_info& type) {
  auto const msg = fmt::format(
      FOLLY_FMT_COMPILE(
          "nullptr_t should be passed if you want {} to be default constructed"),
      folly::StringPiece(demangle(type)));
  throw std::logic_error(msg);
}

[[noreturn]] void singletonThrowGetInvokedAfterDestruction(
    const TypeDescriptor& type) {
  throw std::runtime_error(
      "Raw pointer to a singleton requested after its destruction."
      " Singleton type is: " +
      type.name());
}

} // namespace detail

namespace {

struct FatalHelper {
  ~FatalHelper() {
    if (!leakedSingletons_.empty()) {
      std::string leakedTypes;
      for (const auto& singleton : leakedSingletons_) {
        leakedTypes += "\t" + singleton.name() + "\n";
      }
      LOG(DFATAL)
          << "Singletons of the following types had living references "
          << "after destroyInstances was finished:\n"
          << leakedTypes
          << "beware! It is very likely that those singleton instances "
          << "are leaked.";
    }
  }

  std::vector<detail::TypeDescriptor> leakedSingletons_;
};

#if defined(__APPLE__) || defined(_MSC_VER)
// OS X doesn't support constructor priorities.
FatalHelper fatalHelper;
#else
FatalHelper __attribute__((__init_priority__(101))) fatalHelper;
#endif

} // namespace

SingletonVault::SingletonVault(Type type) noexcept : type_(type) {
  AtFork::registerHandler(
      this,
      /*prepare*/
      [this]() {
        auto singletons = singletons_.rlock();
        auto creationOrder = creationOrder_.rlock();

        CHECK_GE(singletons->size(), creationOrder->size());

        for (const auto& singletonType : *creationOrder) {
          liveSingletonsPreFork_.insert(singletons->at(singletonType));
        }

        return true;
      },
      /*parent*/ [this]() { liveSingletonsPreFork_.clear(); },
      /*child*/
      [this]() {
        for (auto singleton : liveSingletonsPreFork_) {
          singleton->inChildAfterFork();
        }
        liveSingletonsPreFork_.clear();
      });
}

SingletonVault::~SingletonVault() {
  AtFork::unregisterHandler(this);
  destroyInstances();
}

void SingletonVault::registerSingleton(detail::SingletonHolderBase* entry) {
  auto state = state_.rlock();
  state->check(detail::SingletonVaultState::Type::Running);

  if (FOLLY_UNLIKELY(state->registrationComplete) &&
      type_.load(std::memory_order_relaxed) == Type::Strict) {
    LOG(ERROR) << "Registering singleton after registrationComplete().";
  }

  auto singletons = singletons_.wlock();
  CHECK_THROW(
      singletons->emplace(entry->type(), entry).second, std::logic_error);
}

void SingletonVault::addEagerInitSingleton(detail::SingletonHolderBase* entry) {
  auto state = state_.rlock();
  state->check(detail::SingletonVaultState::Type::Running);

  if (FOLLY_UNLIKELY(state->registrationComplete) &&
      type_.load(std::memory_order_relaxed) == Type::Strict) {
    LOG(ERROR) << "Registering for eager-load after registrationComplete().";
  }

  CHECK_THROW(singletons_.rlock()->count(entry->type()), std::logic_error);

  auto eagerInitSingletons = eagerInitSingletons_.wlock();
  eagerInitSingletons->insert(entry);
}

void SingletonVault::addEagerInitOnReenableSingleton(
    detail::SingletonHolderBase* entry) {
  auto state = state_.rlock();
  state->check(detail::SingletonVaultState::Type::Running);

  if (FOLLY_UNLIKELY(state->registrationComplete) &&
      type_.load(std::memory_order_relaxed) == Type::Strict) {
    LOG(ERROR)
        << "Registering for eager-load on re-enable after registrationComplete().";
  }

  CHECK_THROW(singletons_.rlock()->count(entry->type()), std::logic_error);

  auto eagerInitOnReenableSingletons = eagerInitOnReenableSingletons_.wlock();
  eagerInitOnReenableSingletons->insert(entry);
}

void SingletonVault::registrationComplete() {
  scheduleDestroyInstances();

  auto state = state_.wlock();
  state->check(detail::SingletonVaultState::Type::Running);

  if (state->registrationComplete) {
    return;
  }

  auto singletons = singletons_.rlock();
  if (type_.load(std::memory_order_relaxed) == Type::Strict) {
    for (const auto& p : *singletons) {
      if (p.second->hasLiveInstance()) {
        throw std::runtime_error(
            "Singleton " + p.first.name() +
            " created before registration was complete.");
      }
    }
  }

  state->registrationComplete = true;
  singletons.unlock();
  state.unlock();
  if (this == SingletonVault::singleton()) {
    detail::invokeSingletonVaultRegistrationCompleteHooks();
  }
}

void SingletonVault::doEagerInit() {
  {
    auto state = state_.rlock();
    state->check(detail::SingletonVaultState::Type::Running);
    if (FOLLY_UNLIKELY(!state->registrationComplete)) {
      throw std::logic_error("registrationComplete() not yet called");
    }
  }

  auto eagerInitSingletons = eagerInitSingletons_.rlock();
  for (auto* single : *eagerInitSingletons) {
    single->createInstance();
  }
}

void SingletonVault::doEagerInitVia(Executor& exe, folly::Baton<>* done) {
  {
    auto state = state_.rlock();
    state->check(detail::SingletonVaultState::Type::Running);
    if (FOLLY_UNLIKELY(!state->registrationComplete)) {
      throw std::logic_error("registrationComplete() not yet called");
    }
  }

  auto eagerInitSingletons = eagerInitSingletons_.rlock();
  auto countdown =
      std::make_shared<std::atomic<size_t>>(eagerInitSingletons->size());
  for (auto* single : *eagerInitSingletons) {
    // countdown is retained by shared_ptr, and will be alive until last lambda
    // is done.  notifyBaton is provided by the caller, and expected to remain
    // present (if it's non-nullptr).  singletonSet can go out of scope but
    // its values, which are SingletonHolderBase pointers, are alive as long as
    // SingletonVault is not being destroyed.
    exe.add([=] {
      // decrement counter and notify if requested, whether initialization
      // was successful, was skipped (already initialized), or exception thrown.
      SCOPE_EXIT {
        if (--(*countdown) == 0) {
          if (done != nullptr) {
            done->post();
          }
        }
      };
      // if initialization is in progress in another thread, don't try to init
      // here.  Otherwise the current thread will block on 'createInstance'.
      if (!single->creationStarted()) {
        single->createInstance();
      }
    });
  }
}

void SingletonVault::destroyInstances() {
  cancellationSource_.wlock()->requestCancellation();

  auto stateW = state_.wlock();
  if (stateW->state == detail::SingletonVaultState::Type::Quiescing) {
    return;
  }
  stateW->state = detail::SingletonVaultState::Type::Quiescing;

  auto stateR = stateW.moveFromWriteToRead();
  {
    auto singletons = singletons_.rlock();
    auto creationOrder = creationOrder_.rlock();

    CHECK_GE(singletons->size(), creationOrder->size());

    // Release all ReadMostlyMainPtrs at once
    {
      ReadMostlyMainPtrDeleter<> deleter;
      for (auto& singleton_type : *creationOrder) {
        singletons->at(singleton_type)->preDestroyInstance(deleter);
      }
    }

    for (auto type_iter = creationOrder->rbegin();
         type_iter != creationOrder->rend();
         ++type_iter) {
      singletons->at(*type_iter)->destroyInstance();
    }

    for (auto& singleton_type : *creationOrder) {
      auto instance = singletons->at(singleton_type);
      if (!instance->hasLiveInstance()) {
        continue;
      }

      fatalHelper.leakedSingletons_.push_back(instance->type());
    }
  }

  {
    auto creationOrder = creationOrder_.wlock();
    creationOrder->clear();
  }
}

void SingletonVault::reenableInstances() {
  CHECK(!shutdownTimerStarted_.load(std::memory_order_relaxed))
      << "reenableInstances() called after destroyInstancesFinal()";
  {
    auto state = state_.wlock();

    state->check(detail::SingletonVaultState::Type::Quiescing);

    state->state = detail::SingletonVaultState::Type::Running;
  }

  // reset the cancellation source
  cancellationSource_.withWLock([&](auto& cancellationSource) {
    cancellationSource = folly::CancellationSource{};
  });

  auto eagerInitOnReenableSingletons = eagerInitOnReenableSingletons_.copy();
  auto instantiatedAtLeastOnce = instantiatedAtLeastOnce_.copy();
  for (auto* single : eagerInitOnReenableSingletons) {
    if (!instantiatedAtLeastOnce.count(single->type())) {
      continue;
    }
    single->createInstance();
  }
}

void SingletonVault::scheduleDestroyInstances() {
  // Add a dependency on folly::ThreadLocal to make sure all its static
  // singletons are initialized first.
  threadlocal_detail::StaticMeta<void, void>::instance();
#if !defined(FOLLY_SINGLETON_SKIP_SCHEDULE_ATEXIT) || \
    !FOLLY_SINGLETON_SKIP_SCHEDULE_ATEXIT
  std::atexit([] { SingletonVault::singleton()->destroyInstancesFinal(); });
#endif
}

void SingletonVault::destroyInstancesFinal() {
  startShutdownTimer();
  destroyInstances();
}

void SingletonVault::addToShutdownLog(std::string message) {
  std::chrono::time_point<std::chrono::system_clock> now =
      std::chrono::system_clock::now();
  std::chrono::milliseconds millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch());
  shutdownLog_.wlock()->push_back(fmt::format("{:%T} {}", millis, message));
}

#if FOLLY_HAVE_LIBRT
namespace {
[[noreturn]] void fireShutdownSignalHelper(sigval_t sigval) {
  static_cast<SingletonVault*>(sigval.sival_ptr)->fireShutdownTimer();
}
} // namespace
#endif

void SingletonVault::startShutdownTimer() {
#if FOLLY_HAVE_LIBRT
  if (shutdownTimerStarted_.exchange(true)) {
    return;
  }

  if (!shutdownTimeout_.count()) {
    return;
  }

  struct sigevent sig;
  sig.sigev_notify = SIGEV_THREAD;
  sig.sigev_notify_function = fireShutdownSignalHelper;
  sig.sigev_value.sival_ptr = this;
  sig.sigev_notify_attributes = nullptr;
  timer_t timerId = nullptr;
  PCHECK(timer_create(CLOCK_MONOTONIC, &sig, &timerId) == 0);

  struct itimerspec newValue, oldValue;
  newValue.it_value.tv_sec =
      std::chrono::milliseconds(shutdownTimeout_).count() / 1000;
  newValue.it_value.tv_nsec =
      std::chrono::milliseconds(shutdownTimeout_).count() % 1000 * 1000000;
  newValue.it_interval.tv_sec = 0;
  newValue.it_interval.tv_nsec = 0;
  PCHECK(timer_settime(timerId, 0, &newValue, &oldValue) == 0);
#endif
}

[[noreturn]] void SingletonVault::fireShutdownTimer() {
  std::string shutdownLog;
  for (auto& logMessage : shutdownLog_.copy()) {
    shutdownLog += logMessage + "\n";
  }

  auto msg = folly::to<std::string>(
      "Failed to complete shutdown within ",
      std::chrono::milliseconds(shutdownTimeout_).count(),
      "ms. Shutdown log:\n",
      shutdownLog);
  folly::terminate_with<std::runtime_error>(msg);
}

} // namespace folly
