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

namespace folly {
namespace detail {

template <typename TimePoint, typename Duration>
TimePoint roundUpTimePoint(TimePoint t, Duration d) {
  auto remainder = t.time_since_epoch() % d;
  if (remainder.count() != 0) {
    return t + d - remainder;
  }
  return t;
}

template <class T>
void removeEmpty(std::vector<T>& v) {
  const auto& isEmpty = [](const T& val) { return val.empty(); };
  v.erase(std::remove_if(v.begin(), v.end(), isEmpty), v.end());
}

template <typename DigestT, typename ClockT>
BufferedStat<DigestT, ClockT>::BufferedStat(
    typename ClockT::duration bufferDuration,
    size_t bufferSize,
    size_t digestSize)
    : bufferDuration_(bufferDuration),
      expiry_(roundUp(ClockT::now())),
      digestBuilder_(bufferSize, digestSize) {}

template <typename DigestT, typename ClockT>
void BufferedStat<DigestT, ClockT>::append(double value, TimePoint now) {
  if (now > expiry_.load(std::memory_order_relaxed)) [[unlikely]] {
    std::unique_lock g(mutex_, std::try_to_lock_t());
    if (g.owns_lock()) {
      doUpdate(now, g, UpdateMode::OnExpiry);
    }
  }
  digestBuilder_.append(value);
}

template <typename DigestT, typename ClockT>
typename BufferedStat<DigestT, ClockT>::TimePoint
BufferedStat<DigestT, ClockT>::roundUp(TimePoint t) {
  return roundUpTimePoint(t, bufferDuration_);
}

template <typename DigestT, typename ClockT>
std::unique_lock<SharedMutex> BufferedStat<DigestT, ClockT>::updateIfExpired(
    TimePoint now) {
  std::unique_lock g(mutex_);
  doUpdate(now, g, UpdateMode::OnExpiry);
  return g;
}

template <typename DigestT, typename ClockT>
void BufferedStat<DigestT, ClockT>::flush() {
  std::unique_lock g(mutex_);
  doUpdate(ClockT::now(), g, UpdateMode::Now);
}

template <typename DigestT, typename ClockT>
void BufferedStat<DigestT, ClockT>::doUpdate(
    TimePoint now,
    const std::unique_lock<SharedMutex>& g,
    UpdateMode updateMode) {
  assert(g.owns_lock());
  // Check that no other thread has performed the slide after the check
  auto oldExpiry = expiry_.load(std::memory_order_relaxed);
  if (now > oldExpiry || updateMode == UpdateMode::Now) {
    now = roundUp(now);
    expiry_.store(now, std::memory_order_relaxed);
    onNewDigest(digestBuilder_.build(), now, oldExpiry, g);
  }
}

template <typename DigestT, typename ClockT>
BufferedDigest<DigestT, ClockT>::BufferedDigest(
    typename ClockT::duration bufferDuration,
    size_t bufferSize,
    size_t digestSize)
    : BufferedStat<DigestT, ClockT>(bufferDuration, bufferSize, digestSize),
      digest_(digestSize) {}

template <typename DigestT, typename ClockT>
DigestT BufferedDigest<DigestT, ClockT>::get(TimePoint now) {
  auto g = this->updateIfExpired(now);
  return digest_;
}

template <typename DigestT, typename ClockT>
void BufferedDigest<DigestT, ClockT>::onNewDigest(
    DigestT digest,
    TimePoint /*newExpiry*/,
    TimePoint /*oldExpiry*/,
    const std::unique_lock<SharedMutex>& /*g*/) {
  digest_ = DigestT::merge(digest_, digest);
}

template <typename DigestT, typename ClockT>
BufferedSlidingWindow<DigestT, ClockT>::BufferedSlidingWindow(
    size_t nBuckets,
    typename ClockT::duration bufferDuration,
    size_t bufferSize,
    size_t digestSize)
    : BufferedStat<DigestT, ClockT>(bufferDuration, bufferSize, digestSize),
      slidingWindow_([=]() { return DigestT(digestSize); }, nBuckets) {}

template <typename DigestT, typename ClockT>
std::vector<DigestT> BufferedSlidingWindow<DigestT, ClockT>::get(
    TimePoint now) {
  std::vector<DigestT> digests;
  {
    auto g = this->updateIfExpired(now);
    digests = slidingWindow_.get();
  }
  removeEmpty(digests);
  return digests;
}

template <typename DigestT, typename ClockT>
void BufferedSlidingWindow<DigestT, ClockT>::onNewDigest(
    DigestT digest,
    TimePoint newExpiry,
    TimePoint oldExpiry,
    const std::unique_lock<SharedMutex>& /*g*/) {
  if (newExpiry > oldExpiry) {
    auto diff = (newExpiry - oldExpiry) / this->bufferDuration_;
    slidingWindow_.slide(diff);
    slidingWindow_.set(diff - 1, std::move(digest));
  } else {
    // just update current window
    slidingWindow_.set(
        0 /* current window */, DigestT::merge(slidingWindow_.front(), digest));
  }
}

template <typename DigestT, typename ClockT>
BufferedMultiSlidingWindow<DigestT, ClockT>::Window::Window(
    TimePoint firstExpiry,
    std::chrono::seconds bucketDur,
    size_t nBuckets,
    size_t digestSize)
    : bucketDuration(bucketDur),
      expiry(roundUpTimePoint(firstExpiry, bucketDur)),
      curBucket(digestSize),
      slidingWindow([=] { return DigestT(digestSize); }, nBuckets) {}

template <typename DigestT, typename ClockT>
BufferedMultiSlidingWindow<DigestT, ClockT>::BufferedMultiSlidingWindow(
    Range<const WindowDef*> defs, size_t bufferSize, size_t digestSize)
    : BufferedStat<DigestT, ClockT>(
          std::chrono::seconds{1}, bufferSize, digestSize),
      digestSize_(digestSize),
      allTime_(digestSize) {
  for (const auto& def : defs) {
    windows_.emplace_back(
        this->expiry_.load(std::memory_order_relaxed),
        def.first,
        def.second,
        digestSize);
  }
}

template <typename DigestT, typename ClockT>
auto BufferedMultiSlidingWindow<DigestT, ClockT>::get(TimePoint now)
    -> Digests {
  auto digests = [&] {
    std::vector<std::vector<DigestT>> windowDigests;
    windowDigests.reserve(windows_.size());
    auto g = this->updateIfExpired(now);
    for (const auto& window : windows_) {
      windowDigests.push_back(window.slidingWindow.get());
    }
    return Digests{allTime_, std::move(windowDigests)};
  }();

  for (auto& w : digests.windows) {
    removeEmpty(w);
  }
  return digests;
}

template <typename DigestT, typename ClockT>
void BufferedMultiSlidingWindow<DigestT, ClockT>::onNewDigest(
    DigestT digest,
    TimePoint newExpiry,
    TimePoint oldExpiry,
    const std::unique_lock<SharedMutex>& /* g */) {
  for (auto& window : windows_) {
    assert(oldExpiry <= window.expiry);
    auto& curBucket = window.curBucket;
    auto& slidingWindow = window.slidingWindow;
    if (newExpiry > oldExpiry) {
      curBucket = DigestT::merge(curBucket, digest);
      if (newExpiry > window.expiry) {
        auto newWindowExpiry =
            roundUpTimePoint(newExpiry, window.bucketDuration);
        auto diff = (newWindowExpiry - window.expiry) / window.bucketDuration;
        slidingWindow.slide(diff);
        slidingWindow.set(
            diff - 1, std::exchange(curBucket, DigestT{digestSize_}));
        window.expiry = newWindowExpiry;
      }
    } else {
      // This is a flush, merge curBucket in even if it hasn't expired.
      std::array<const DigestT*, 3> a{
          &slidingWindow.front(), &curBucket, &digest};
      slidingWindow.set(0, DigestT::merge(range(a)));
      curBucket = DigestT{digestSize_};
    }
  }

  allTime_ = DigestT::merge(allTime_, digest);
}

} // namespace detail
} // namespace folly
