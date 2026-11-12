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

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <tuple>
#include <utility>

#include <glog/logging.h>

namespace folly {

namespace detail {

struct MergingCancellationStateTag {};

// Internal cancellation state object.
class CancellationState {
 public:
  [[nodiscard]] static CancellationStateSourcePtr create();

 protected:
  // Constructed initially with a CancellationSource reference count of 1.
  CancellationState() noexcept;
  // Constructed initially with a CancellationToken reference count of 1.
  explicit CancellationState(MergingCancellationStateTag) noexcept;

  virtual ~CancellationState();

  friend struct CancellationStateTokenDeleter;
  friend struct CancellationStateSourceDeleter;

  void removeTokenReference() noexcept;
  void removeSourceReference() noexcept;

 public:
  [[nodiscard]] CancellationStateTokenPtr addTokenReference() noexcept;

  [[nodiscard]] CancellationStateSourcePtr addSourceReference() noexcept;

  bool tryAddCallback(
      CancellationCallback* callback,
      bool incrementRefCountIfSuccessful) noexcept;

  void removeCallback(CancellationCallback* callback) noexcept;

  bool isCancellationRequested() const noexcept;
  bool canBeCancelled() const noexcept;

  // Request cancellation.
  // Return 'true' if cancellation had already been requested.
  // Return 'false' if this was the first thread to request
  // cancellation.
  bool requestCancellation() noexcept;

 private:
  void lock() noexcept;
  void unlock() noexcept;
  void unlockAndIncrementTokenCount() noexcept;
  void unlockAndDecrementTokenCount() noexcept;
  bool tryLockAndCancelUnlessCancelled() noexcept;

  template <typename Predicate>
  bool tryLock(Predicate predicate) noexcept;

  static bool canBeCancelled(std::uint64_t state) noexcept;
  static bool isCancellationRequested(std::uint64_t state) noexcept;
  static bool isLocked(std::uint64_t state) noexcept;

  static constexpr std::uint64_t kCancellationRequestedFlag = 1;
  static constexpr std::uint64_t kLockedFlag = 2;
  static constexpr std::uint64_t kMergingFlag = 4;
  static constexpr std::uint64_t kTokenReferenceCountIncrement = 8;
  static constexpr std::uint64_t kSourceReferenceCountIncrement =
      std::uint64_t(1) << 34u;
  static constexpr std::uint64_t kTokenReferenceCountMask =
      (kSourceReferenceCountIncrement - 1u) -
      (kTokenReferenceCountIncrement - 1u);
  static constexpr std::uint64_t kSourceReferenceCountMask =
      std::numeric_limits<std::uint64_t>::max() -
      (kSourceReferenceCountIncrement - 1u);

  // Bit 0 - Cancellation Requested
  // Bit 1 - Locked Flag
  // Bit 2 - MergingCancellationState Flag
  // Bits 3-33  - Token reference count (max ~2 billion)
  // Bits 34-63 - Source reference count (max ~1 billion)
  std::atomic<std::uint64_t> state_;
  CancellationCallback* head_{nullptr};
  std::thread::id signallingThreadId_;
};

template <typename... Data>
class CancellationStateWithData : public CancellationState {
  template <typename... Args>
  CancellationStateWithData(Args&&... data);

 public:
  template <typename... Args>
  [[nodiscard]] static std::
      pair<CancellationStateSourcePtr, std::tuple<Data...>*>
      create(Args&&... data);

 private:
  std::tuple<Data...> data_;
};

inline void CancellationStateTokenDeleter::operator()(
    CancellationState* state) noexcept {
  state->removeTokenReference();
}

inline void CancellationStateSourceDeleter::operator()(
    CancellationState* state) noexcept {
  state->removeSourceReference();
}

} // namespace detail

inline CancellationToken::CancellationToken(
    const CancellationToken& other) noexcept
    : state_() {
  if (other.state_) {
    state_ = other.state_->addTokenReference();
  }
}

inline CancellationToken::CancellationToken(CancellationToken&& other) noexcept
    : state_(std::move(other.state_)) {}

inline CancellationToken& CancellationToken::operator=(
    const CancellationToken& other) noexcept {
  if (state_ != other.state_) {
    CancellationToken temp{other};
    swap(temp);
  }
  return *this;
}

inline CancellationToken& CancellationToken::operator=(
    CancellationToken&& other) noexcept {
  state_ = std::move(other.state_);
  return *this;
}

inline bool CancellationToken::isCancellationRequested() const noexcept {
  return state_ != nullptr && state_->isCancellationRequested();
}

inline bool CancellationToken::canBeCancelled() const noexcept {
  return state_ != nullptr && state_->canBeCancelled();
}

inline void CancellationToken::swap(CancellationToken& other) noexcept {
  std::swap(state_, other.state_);
}

inline CancellationToken::CancellationToken(
    detail::CancellationStateTokenPtr state) noexcept
    : state_(std::move(state)) {}

inline bool operator==(
    const CancellationToken& a, const CancellationToken& b) noexcept {
  return a.state_ == b.state_;
}

inline CancellationSource::CancellationSource()
    : state_(detail::CancellationState::create()) {}

inline CancellationSource::CancellationSource(
    const CancellationSource& other) noexcept
    : state_() {
  if (other.state_) {
    state_ = other.state_->addSourceReference();
  }
}

inline CancellationSource::CancellationSource(
    CancellationSource&& other) noexcept
    : state_(std::move(other.state_)) {}

inline CancellationSource& CancellationSource::operator=(
    const CancellationSource& other) noexcept {
  if (state_ != other.state_) {
    CancellationSource temp{other};
    swap(temp);
  }
  return *this;
}

inline CancellationSource& CancellationSource::operator=(
    CancellationSource&& other) noexcept {
  state_ = std::move(other.state_);
  return *this;
}

inline CancellationSource CancellationSource::invalid() noexcept {
  return CancellationSource{detail::CancellationStateSourcePtr{}};
}

inline bool CancellationSource::isCancellationRequested() const noexcept {
  return state_ != nullptr && state_->isCancellationRequested();
}

inline bool CancellationSource::canBeCancelled() const noexcept {
  return state_ != nullptr;
}

inline CancellationToken CancellationSource::getToken() const noexcept {
  if (state_ != nullptr) {
    return CancellationToken{state_->addTokenReference()};
  }
  return CancellationToken{};
}

inline bool CancellationSource::requestCancellation() const noexcept {
  if (state_ != nullptr) {
    return state_->requestCancellation();
  }
  return false;
}

inline void CancellationSource::swap(CancellationSource& other) noexcept {
  std::swap(state_, other.state_);
}

inline CancellationSource::CancellationSource(
    detail::CancellationStateSourcePtr&& state) noexcept
    : state_(std::move(state)) {}

template <typename Callable>
  requires std::is_constructible<CancellationCallback::VoidFunction, Callable>::
               value
inline CancellationCallback::CancellationCallback(
    CancellationToken&& ct, Callable&& callable)
    : next_(nullptr),
      prevNext_(nullptr),
      state_(nullptr),
      callback_(static_cast<Callable&&>(callable)),
      destructorHasRunInsideCallback_(nullptr),
      callbackCompleted_(false) {
  if (ct.state_ != nullptr && ct.state_->tryAddCallback(this, false)) {
    state_ = ct.state_.release();
  }
}

template <typename Callable>
  requires std::is_constructible<CancellationCallback::VoidFunction, Callable>::
               value
inline CancellationCallback::CancellationCallback(
    const CancellationToken& ct, Callable&& callable)
    : next_(nullptr),
      prevNext_(nullptr),
      state_(nullptr),
      callback_(static_cast<Callable&&>(callable)),
      destructorHasRunInsideCallback_(nullptr),
      callbackCompleted_(false) {
  if (ct.state_ != nullptr && ct.state_->tryAddCallback(this, true)) {
    state_ = ct.state_.get();
  }
}

inline CancellationCallback::~CancellationCallback() {
  if (state_ != nullptr) {
    state_->removeCallback(this);
  }
}

inline void CancellationCallback::invokeCallback() noexcept {
  // Invoke within a noexcept context so that we std::terminate() if it throws.
  callback_();
}

namespace detail {

inline CancellationStateSourcePtr CancellationState::create() {
  return CancellationStateSourcePtr{new CancellationState()};
}

inline CancellationState::CancellationState() noexcept
    : state_(kSourceReferenceCountIncrement) {}
inline CancellationState::CancellationState(
    MergingCancellationStateTag) noexcept
    : state_(kTokenReferenceCountIncrement | kMergingFlag) {}

inline CancellationStateTokenPtr
CancellationState::addTokenReference() noexcept {
  state_.fetch_add(kTokenReferenceCountIncrement, std::memory_order_relaxed);
  return CancellationStateTokenPtr{this};
}

// This `alignas()` is here because "create" will going to allocate this
// back-to-back with our `CancellationCallback` array.
class alignas(CancellationState) alignas(CancellationCallback)
    MergingCancellationState : public CancellationState {
  // There's a noticeable perf gain from specializing the constructors
  struct CopyTag {};
  struct MoveTag {};
  struct CopyMoveTag {};

  explicit MergingCancellationState();

  CancellationCallback* callbackEnd_; // byte after the last callback

 public:
  // Ctors are a private implementation detail of the create*() factory funcs
  explicit MergingCancellationState(
      CopyTag, size_t nCopy, const CancellationToken** copyToks);
  explicit MergingCancellationState(
      MoveTag, size_t nMove, CancellationToken** moveToks);
  explicit MergingCancellationState(
      CopyMoveTag,
      size_t nCopy,
      const CancellationToken** copyToks,
      size_t nMove,
      CancellationToken** moveToks);
  ~MergingCancellationState() override;

  static CancellationStateTokenPtr createCopy(
      size_t nCopy, const CancellationToken** copyToks);
  static CancellationStateTokenPtr createMove(
      size_t nMove, CancellationToken** moveToks);
  static CancellationStateTokenPtr createCopyMove(
      size_t nCopy,
      const CancellationToken** copyToks,
      size_t nMove,
      CancellationToken** moveToks);
  void destroy() noexcept;
};

inline void CancellationState::removeTokenReference() noexcept {
  const auto oldState = state_.fetch_sub(
      kTokenReferenceCountIncrement, std::memory_order_acq_rel);
  DCHECK(
      (oldState & kTokenReferenceCountMask) >= kTokenReferenceCountIncrement);
  if (oldState < (2 * kTokenReferenceCountIncrement)) {
    if (oldState & kMergingFlag) {
      static_cast<MergingCancellationState*>(this)->destroy();
    } else {
      delete this;
    }
  }
}

inline CancellationStateSourcePtr
CancellationState::addSourceReference() noexcept {
  state_.fetch_add(kSourceReferenceCountIncrement, std::memory_order_relaxed);
  return CancellationStateSourcePtr{this};
}

inline void CancellationState::removeSourceReference() noexcept {
  const auto oldState = state_.fetch_sub(
      kSourceReferenceCountIncrement, std::memory_order_acq_rel);
  DCHECK(
      (oldState & kSourceReferenceCountMask) >= kSourceReferenceCountIncrement);
  if (oldState <
      (kSourceReferenceCountIncrement + kTokenReferenceCountIncrement)) {
    // No "free()" branch because "merging" state has no source pointers.
    DCHECK(!(oldState & kMergingFlag));
    delete this;
  }
}

inline bool CancellationState::isCancellationRequested() const noexcept {
  return isCancellationRequested(state_.load(std::memory_order_acquire));
}

inline bool CancellationState::canBeCancelled() const noexcept {
  return canBeCancelled(state_.load(std::memory_order_acquire));
}

inline bool CancellationState::canBeCancelled(std::uint64_t state) noexcept {
  // Can be cancelled if there is at least one CancellationSource ref-count
  // or if cancellation has been requested.
  return (state >= kSourceReferenceCountIncrement) ||
      (state & kMergingFlag) != 0 || isCancellationRequested(state);
}

inline bool CancellationState::isCancellationRequested(
    std::uint64_t state) noexcept {
  return (state & kCancellationRequestedFlag) != 0;
}

inline bool CancellationState::isLocked(std::uint64_t state) noexcept {
  return (state & kLockedFlag) != 0;
}

template <typename... Data>
struct WithDataTag {};

template <typename... Data>
template <typename... Args>
CancellationStateWithData<Data...>::CancellationStateWithData(Args&&... data)
    : data_(std::forward<Args>(data)...) {}

template <typename... Data>
template <typename... Args>
std::pair<CancellationStateSourcePtr, std::tuple<Data...>*>
CancellationStateWithData<Data...>::create(Args&&... data) {
  auto* state =
      new CancellationStateWithData<Data...>(std::forward<Args>(data)...);
  return {CancellationStateSourcePtr{state}, &state->data_};
}

} // namespace detail

template <typename... Data, typename... Args>
std::pair<CancellationSource, std::tuple<Data...>*> CancellationSource::create(
    detail::WithDataTag<Data...>, Args&&... data) {
  auto cancellationStateWithData =
      detail::CancellationStateWithData<Data...>::create(
          std::forward<Args>(data)...);
  return {
      CancellationSource{std::move(cancellationStateWithData.first)},
      cancellationStateWithData.second};
}

template <typename... Ts>
CancellationToken cancellation_token_merge_fn::operator()(
    Ts&&... tokens) const {
  if constexpr (sizeof...(Ts) == 0) {
    return CancellationToken();
  } else if constexpr (sizeof...(Ts) == 1) {
    return (tokens, ...);
  } else {
    constexpr size_t N = sizeof...(Ts);
    constexpr size_t NCopy =
        ((std::is_reference_v<Ts> || std::is_const_v<Ts>)+...);
    std::array<const CancellationToken*, NCopy> copyToks;
    std::array<CancellationToken*, N - NCopy> moveToks;
    const detail::CancellationState* prevState = nullptr;
    size_t copyIdx = 0, moveIdx = 0;
    (
        [&] {
          constexpr bool mustCopy =
              std::is_reference_v<Ts> || std::is_const_v<Ts>;
          auto* state = tokens.state_.get();
          if (!state) {
            return; // Omit empties
          }
          if (state == prevState) {
            if constexpr (!mustCopy) {
              // Move out the input token, although we didn't use it.  The
              // goal is to make deduplication non-observable by the user.
              std::exchange(tokens, {});
            }
            return; // Omit adjacent duplicates
          }
          prevState = state;
          if constexpr (mustCopy) {
            copyToks[copyIdx++] = &tokens;
          } else {
            moveToks[moveIdx++] = &tokens;
          }
        }(),
        ...);

    size_t n = copyIdx + moveIdx;
    if (n == 0) {
      return CancellationToken();
    } else if (n == 1) {
      if (moveIdx) { // A ternary would have type `const CT*` and NOT move!
        return std::move(*moveToks[0]);
      }
      return *copyToks[0];
    } else if constexpr (NCopy == N) {
      return CancellationToken(
          detail::MergingCancellationState::createCopy(
              copyIdx, copyToks.data()));
    } else if constexpr (NCopy == 0) {
      return CancellationToken(
          detail::MergingCancellationState::createMove(
              moveIdx, moveToks.data()));
    } else {
      return CancellationToken(
          detail::MergingCancellationState::createCopyMove(
              copyIdx, copyToks.data(), moveIdx, moveToks.data()));
    }
  }
}

} // namespace folly
