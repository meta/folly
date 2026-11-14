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

#include <glog/logging.h>

#include <folly/Function.h>
#include <folly/portability/Config.h>

#if defined(FOLLY_FIBERS_USE_BOOST_CONTEXT) && FOLLY_FIBERS_USE_BOOST_CONTEXT
#include <boost/context/detail/fcontext.hpp> // @manual
#else
#include <folly/external/boost-fcontext/fcontext.h>
#endif

#if defined(__clang__)
#define FOLLY_DETAIL_DISABLE_TAIL_CALLS __attribute__((disable_tail_calls))
#define FOLLY_DETAIL_TAIL_CALLS_CAN_BE_DISABLED 1
#elif defined(__GNUC__)
#define FOLLY_DETAIL_DISABLE_TAIL_CALLS \
  __attribute__((optimize("no-optimize-sibling-calls")))
#define FOLLY_DETAIL_TAIL_CALLS_CAN_BE_DISABLED 1
#else
#define FOLLY_DETAIL_DISABLE_TAIL_CALLS
#define FOLLY_DETAIL_TAIL_CALLS_CAN_BE_DISABLED 0
#endif

namespace folly {
namespace fibers {

namespace fcontext {
#if defined(FOLLY_FIBERS_USE_BOOST_CONTEXT) && FOLLY_FIBERS_USE_BOOST_CONTEXT
using boost::context::detail::fcontext_t;
using boost::context::detail::jump_fcontext;
using boost::context::detail::make_fcontext;
using boost::context::detail::transfer_t;
#else
using folly::detail::fcontext::fcontext_t;
using folly::detail::fcontext::transfer_t;
inline constexpr auto jump_fcontext =
    &folly::detail::fcontext::folly_jump_fcontext;
inline constexpr auto make_fcontext =
    &folly::detail::fcontext::folly_make_fcontext;
#endif
} // namespace fcontext

class FiberImpl {
  using FiberContext = fcontext::fcontext_t;

  using MainContext = fcontext::fcontext_t;

 public:
  FiberImpl(
      folly::Function<void()> func, unsigned char* stackLimit, size_t stackSize)
      : func_(std::move(func)) {
    auto stackBase = stackLimit + stackSize;
    entryFrameBase_ = nullptr;
    fiberContext_ = fcontext::make_fcontext(stackBase, stackSize, &fiberFunc);
  }

  void activate() {
    auto transfer = fcontext::jump_fcontext(fiberContext_, this);
    fiberContext_ = transfer.fctx;
    auto context = reinterpret_cast<intptr_t>(transfer.data);
    DCHECK_EQ(0, context);
  }

  void deactivate() {
    auto transfer = fcontext::jump_fcontext(mainContext_, nullptr);
    mainContext_ = transfer.fctx;
    fixStackUnwinding();
    auto context = reinterpret_cast<intptr_t>(transfer.data);
    DCHECK_EQ(this, reinterpret_cast<FiberImpl*>(context));
  }

  void* getStackPointer() const {
    if constexpr (kIsLinux || kIsApple) {
      if constexpr (kIsArchAmd64) {
        // RBP at offset 0x30 (index 6) in the SysV x86_64 fcontext layout.
        return reinterpret_cast<void**>(fiberContext_)[6];
      } else if constexpr (kIsArchAArch64) {
        // FP (x29) at offset 0x90 (index 18) in the arm64 fcontext layout.
        return reinterpret_cast<void**>(fiberContext_)[18];
      }
    }
    return nullptr;
  }

 private:
  static FOLLY_NOINLINE FOLLY_DETAIL_DISABLE_TAIL_CALLS void fiberFunc(
      fcontext::transfer_t transfer) {
    auto fiberImpl = reinterpret_cast<FiberImpl*>(transfer.data);
    fiberImpl->mainContext_ = transfer.fctx;
#if FOLLY_HAS_BUILTIN(__builtin_frame_address) && \
    FOLLY_DETAIL_TAIL_CALLS_CAN_BE_DISABLED
    fiberImpl->entryFrameBase_ = __builtin_frame_address(0);
#endif
    fiberImpl->fixStackUnwinding();
    fiberImpl->func_();
  }

  void fixStackUnwinding() {
    if (kIsLinux || kIsApple) {
      // Stitch main context stack and fiber stack so that frame-pointer-based
      // stack walkers can terminate cleanly at the fiber boundary on the
      // ELF and Mach-O fcontext backends.
      auto frameBase = reinterpret_cast<void**>(entryFrameBase_);
      if (frameBase == nullptr) {
        return;
      }
      auto mainContext = reinterpret_cast<void**>(mainContext_);
      if constexpr (kIsArchAmd64) {
        // Extract RBP and RIP from main context (offsets 6 and 7).
        frameBase[0] = mainContext[6];
        frameBase[1] = mainContext[7];
      } else if constexpr (kIsArchAArch64) {
        // Extract FP (x29) and LR (x30) from main context
        // (offsets 0x90 and 0x98 in the fcontext layout).
        frameBase[0] = mainContext[18];
        frameBase[1] = mainContext[19];
      }
    }
  }

  void* entryFrameBase_;
  folly::Function<void()> func_;
  FiberContext fiberContext_;
  MainContext mainContext_;
};
} // namespace fibers
} // namespace folly

#undef FOLLY_DETAIL_TAIL_CALLS_CAN_BE_DISABLED
#undef FOLLY_DETAIL_DISABLE_TAIL_CALLS
