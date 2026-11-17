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
#include <cstddef>

#include <glog/logging.h>
#include <folly/Utility.h>
#include <folly/portability/SysMman.h>
#include <folly/tracing/AsyncStack.h>

namespace folly::test {

template <typename T>
class MappedPage : private folly::NonCopyableNonMovable {
 public:
  explicit MappedPage(int protection)
      : memory_(mmap(
            nullptr,
            sizeof(T),
            protection,
            MAP_PRIVATE | MAP_ANONYMOUS,
            -1,
            0)) {
    PCHECK(memory_ != MAP_FAILED);
  }

  ~MappedPage() { PCHECK(munmap(memory_, sizeof(T)) == 0); }

  T* get() const noexcept { return static_cast<T*>(memory_); }

  void protectOrFatal(int protection) {
    PCHECK(mprotect(memory_, sizeof(T), protection) == 0);
  }

 private:
  void* memory_;
};

// Link the frames before they become visible; keep the last frame's parent.
template <typename... Rest>
inline void linkInactiveFrames(
    AsyncStackFrame& first, AsyncStackFrame& second, Rest&... rest) noexcept {
  std::array<AsyncStackFrame*, 2 + sizeof...(Rest)> frames{
      &first, &second, &rest...};
  for (std::size_t i = 1; i < frames.size(); ++i) {
    frames[i - 1]->setParentFrameUnsafe(*frames[i]);
  }
}

class ScopedAsyncStackRootWithFrame {
 public:
  // Default arguments attach the root to the caller's native stack frame.
  explicit ScopedAsyncStackRootWithFrame(
      AsyncStackFrame& frame,
      void* framePointer = FOLLY_ASYNC_STACK_FRAME_POINTER(),
      void* returnAddress = FOLLY_ASYNC_STACK_RETURN_ADDRESS()) noexcept
      : root_(framePointer, returnAddress), frame_(frame) {
    root_.activateFrame(frame_);
  }

  ~ScopedAsyncStackRootWithFrame() { deactivateAsyncStackFrame(frame_); }

 private:
  folly::detail::ScopedAsyncStackRoot root_;
  AsyncStackFrame& frame_;
};

#if FOLLY_HAS_ASYNC_STACK_METADATA
inline void markAsMetadataFrame(AsyncStackFrame& frame) noexcept {
  frame.setReturnAddress(
      // NOLINTNEXTLINE(performance-no-int-to-ptr): recreate a test marker
      reinterpret_cast<void*>(folly_async_stack_metadata_frame_cookie));
}
#endif

} // namespace folly::test
