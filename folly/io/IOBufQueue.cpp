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

#include <folly/io/IOBufQueue.h>

#include <cstring>
#include <stdexcept>

using std::make_pair;
using std::pair;
using std::unique_ptr;

namespace {

using folly::IOBuf;

const size_t MIN_ALLOC_SIZE = 2000;
const size_t MAX_ALLOC_SIZE = 8000;

/**
 * Convenience functions to append chain src to chain dst.
 */
template <class Src, class Next>
void packInto(IOBuf* tail, Src& src, Next next) {
  if (tail->isSharedOne()) {
    return;
  }

  // Copy up to kMaxPackCopy bytes if we can free buffers; this helps reduce
  // waste (the tail's tailroom and the head's headroom) when joining two
  // IOBufQueues together.
  size_t copyRemaining = folly::IOBufQueue::kMaxPackCopy;
  std::size_t n;
  while (src && (n = src->length()) <= copyRemaining && n <= tail->tailroom()) {
    if (n > 0) {
      memcpy(tail->writableTail(), src->data(), n);
      tail->append(n);
      copyRemaining -= n;
    }
    src = next(std::move(src));
  }
}

void appendToChain(unique_ptr<IOBuf>& dst, unique_ptr<IOBuf>&& src, bool pack) {
  if (dst == nullptr) {
    dst = std::move(src);
  } else {
    IOBuf* tail = dst->prev();
    if (pack) {
      packInto(tail, src, [](auto&& cur) { return cur->pop(); });
    }
    if (src) {
      tail->insertAfterThisOne(std::move(src));
    }
  }
}

} // namespace

namespace folly {

IOBufQueue::IOBufQueue(const Options& options)
    : options_(options), cachePtr_(&localCache_) {
  localCache_.attached = true;
}

IOBufQueue::~IOBufQueue() {
  // Detach an external WritableRangeCache so its destructor does not later
  // flush through the now-dangling queue pointer. Skip flushing otherwise:
  // the bookkeeping is about to be discarded with head_.
  if (cachePtr_ != &localCache_) {
    *cachePtr_ = WritableRangeCacheData{};
  }
}

IOBufQueue::IOBufQueue(IOBufQueue&& other) noexcept
    : options_(other.options_), cachePtr_(&localCache_) {
  other.clearWritableRangeCache();

  head_ = std::move(other.head_);
  factory_ = std::exchange(other.factory_, nullptr);

  tailStart_ = std::exchange(other.tailStart_, nullptr);
  localCache_.cachedRange =
      std::exchange(other.localCache_.cachedRange, {nullptr, nullptr});
  localCache_.attached = true;
  chainLength_ = std::exchange(other.chainLength_, 0);
}

IOBufQueue& IOBufQueue::operator=(IOBufQueue&& other) noexcept {
  if (&other != this) {
    other.clearWritableRangeCache();
    clearWritableRangeCache();

    options_ = other.options_;
    head_ = std::move(other.head_);
    factory_ = std::exchange(other.factory_, nullptr);

    tailStart_ = std::exchange(other.tailStart_, nullptr);
    localCache_.cachedRange =
        std::exchange(other.localCache_.cachedRange, {nullptr, nullptr});
    localCache_.attached = true;
    chainLength_ = std::exchange(other.chainLength_, 0);
  }
  return *this;
}

std::pair<void*, std::size_t> IOBufQueue::headroom() {
  // Note, headroom is independent from the tail, so we don't need to flush the
  // cache.
  if (head_) {
    return std::make_pair(head_->writableBuffer(), head_->headroom());
  } else {
    return std::make_pair(nullptr, 0);
  }
}

void IOBufQueue::markPrepended(std::size_t n) {
  if (n == 0) {
    return;
  }
  // Note, headroom is independent from the tail, so we don't need to flush the
  // cache.
  assert(head_);
  head_->prepend(n);
  chainLength_ += n;
}

void IOBufQueue::prepend(const void* buf, std::size_t n) {
  // We're not touching the tail, so we don't need to flush the cache.
  auto hroom = head_->headroom();
  if (!head_ || hroom < n) {
    throw std::overflow_error("Not enough room to prepend");
  }
  memcpy(head_->writableBuffer() + hroom - n, buf, n);
  head_->prepend(n);
  chainLength_ += n;
}

void IOBufQueue::append(
    unique_ptr<IOBuf>&& buf, bool pack, bool allowTailReuse) {
  if (!buf) {
    return;
  }
  auto guard = updateGuard(allowTailReuse);
  if (options_.cacheChainLength) {
    chainLength_ += buf->computeChainDataLength();
  }
  appendToChain(head_, std::move(buf), pack);
}

void IOBufQueue::append(
    const folly::IOBuf& buf, bool pack, bool allowTailReuse) {
  if (!head_ || !pack) {
    append(buf.clone(), pack, allowTailReuse);
    return;
  }

  auto guard = updateGuard(allowTailReuse);
  if (options_.cacheChainLength) {
    chainLength_ += buf.computeChainDataLength();
  }

  folly::IOBuf* tail = head_->prev();
  const folly::IOBuf* src = &buf;
  packInto(tail, src, [&](auto&& cur) {
    auto next = cur->next();
    return next != &buf ? next : nullptr;
  });
  if (!src) {
    return; // Consumed full input.
  }

  // Clone the rest.
  do {
    head_->appendToChain(src->cloneOne());
    src = src->next();
  } while (src != &buf);
}

void IOBufQueue::append(folly::IOBuf&& buf, bool pack, bool allowTailReuse) {
  // Equivalent to append(std::make_unique<folly::IOBuf>(std::move(buf)), ...)
  // but that would make an unnecessary allocation if buf can be completely be
  // packed into the tail, so we make sure to handle that case.
  auto guard = updateGuard(allowTailReuse);
  if (options_.cacheChainLength) {
    chainLength_ += buf.computeChainDataLength();
  }

  std::unique_ptr<folly::IOBuf> rest;
  if (head_ && pack) {
    folly::IOBuf* src = &buf;
    folly::IOBuf* tail = head_->prev();
    packInto(tail, src, [&](auto* cur) {
      rest = cur->pop();
      return rest.get();
    });
    if (!src) {
      return; // Consumed full input.
    }
    DCHECK(rest == nullptr || rest.get() == src);
  }

  if (!rest) {
    // buf's head was not popped, so we need to heap-allocate it.
    rest = std::make_unique<folly::IOBuf>(std::move(buf));
  }

  if (!head_) {
    head_ = std::move(rest);
  } else {
    head_->appendToChain(std::move(rest));
  }
}

void IOBufQueue::append(IOBufQueue& other, bool pack, bool allowTailReuse) {
  if (!other.head_) {
    return;
  }
  // We're going to chain other, thus we need to grab both guards.
  auto otherGuard = other.updateGuard(allowTailReuse);
  auto guard = updateGuard();
  if (options_.cacheChainLength) {
    if (other.options_.cacheChainLength) {
      chainLength_ += other.chainLength_;
    } else {
      chainLength_ += other.head_->computeChainDataLength();
    }
  }
  appendToChain(head_, std::move(other.head_), pack);
  other.chainLength_ = 0;
}

void IOBufQueue::append(const void* buf, size_t len) {
  auto guard = updateGuard();
  auto src = static_cast<const uint8_t*>(buf);
  while (len != 0) {
    if ((head_ == nullptr) || head_->prev()->isSharedOne() ||
        (head_->prev()->tailroom() == 0)) {
      appendToChain(
          head_,
          createBuf(std::max(MIN_ALLOC_SIZE, std::min(len, MAX_ALLOC_SIZE))),
          false);
    }
    IOBuf* last = head_->prev();
    std::size_t copyLen = std::min(len, (size_t)last->tailroom());
    memcpy(last->writableTail(), src, copyLen);
    src += copyLen;
    last->append(copyLen);
    chainLength_ += copyLen;
    len -= copyLen;
  }
}

void IOBufQueue::wrapBuffer(
    const void* buf, size_t len, std::size_t blockSize) {
  auto src = static_cast<const uint8_t*>(buf);
  while (len != 0) {
    size_t n = std::min(len, size_t(blockSize));
    append(IOBuf::wrapBuffer(src, n));
    src += n;
    len -= n;
  }
}

pair<void*, std::size_t> IOBufQueue::preallocateSlow(
    std::size_t min, std::size_t newAllocationSize, std::size_t max) {
  // Avoid grabbing update guard, since we're manually setting the cache ptrs.
  flushCache();
  // Allocate a new buffer of the requested max size.
  unique_ptr<IOBuf> newBuf(createBuf(std::max(min, newAllocationSize)));

  tailStart_ = newBuf->writableTail();
  cachePtr_->cachedRange = std::pair<uint8_t*, uint8_t*>(
      tailStart_, tailStart_ + newBuf->tailroom());
  appendToChain(head_, std::move(newBuf), false);
  return make_pair(writableTail(), std::min<std::size_t>(max, tailroom()));
}

void IOBufQueue::maybeReuseTail(folly::IOBuf& oldTail) {
  if (oldTail.isSharedOne() || // Can't reuse a shared IOBuf.
      &oldTail == head_->prev() || // No new IOBufs were appended.
      // New tail IOBuf has at least as much tailroom and is writable.
      (head_->prev()->tailroom() >= oldTail.tailroom() &&
       !head_->prev()->isSharedOne())) {
    return;
  }

  std::unique_ptr<IOBuf> newTail;
  if (oldTail.length() == 0) {
    // Nothing was written to the old tail, we can just move it to the end.
    if (&oldTail == head_.get()) {
      newTail = std::exchange(head_, head_->pop());
    } else {
      newTail = oldTail.unlink();
    }
  } else {
    newTail = oldTail.maybeSplitTail();
    if (!newTail) {
      return;
    }
  }
  head_->appendToChain(std::move(newTail));
}

unique_ptr<IOBuf> IOBufQueue::split(
    size_t n, bool throwOnUnderflow, bool pack) {
  auto guard = updateGuard();
  unique_ptr<IOBuf> result;
  while (n != 0) {
    if (head_ == nullptr) [[unlikely]] {
      if (throwOnUnderflow) {
        throw std::underflow_error(
            "Attempt to remove more bytes than are present in IOBufQueue");
      } else {
        break;
      }
    } else if (head_->length() <= n) {
      n -= head_->length();
      chainLength_ -= head_->length();
      unique_ptr<IOBuf> remainder = head_->pop();
      appendToChain(result, std::move(head_), pack);
      head_ = std::move(remainder);
    } else {
      unique_ptr<IOBuf> clone = head_->cloneOne();
      clone->trimEnd(clone->length() - n);
      appendToChain(result, std::move(clone), false);
      head_->trimStart(n);
      chainLength_ -= n;
      break;
    }
  }
  if (result == nullptr) [[unlikely]] {
    return createBuf(0);
  }
  return result;
}

void IOBufQueue::trimStart(size_t amount) {
  auto trimmed = trimStartAtMost(amount);
  if (trimmed != amount) {
    throw std::underflow_error(
        "Attempt to trim more bytes than are present in IOBufQueue");
  }
}

size_t IOBufQueue::trimStartAtMost(size_t amount) {
  auto guard = updateGuard();
  auto original = amount;
  while (amount > 0) {
    if (!head_) {
      break;
    }
    if (head_->length() > amount) {
      head_->trimStart(amount);
      chainLength_ -= amount;
      amount = 0;
      break;
    }
    amount -= head_->length();
    chainLength_ -= head_->length();
    head_ = head_->pop();
  }
  return original - amount;
}

void IOBufQueue::trimEnd(size_t amount) {
  auto trimmed = trimEndAtMost(amount);
  if (trimmed != amount) {
    throw std::underflow_error(
        "Attempt to trim more bytes than are present in IOBufQueue");
  }
}

size_t IOBufQueue::trimEndAtMost(size_t amount) {
  auto guard = updateGuard();
  auto original = amount;
  while (amount > 0) {
    if (!head_) {
      break;
    }
    if (head_->prev()->length() > amount) {
      head_->prev()->trimEnd(amount);
      chainLength_ -= amount;
      amount = 0;
      break;
    }
    amount -= head_->prev()->length();
    chainLength_ -= head_->prev()->length();

    if (head_->isChained()) {
      head_->prev()->unlink();
    } else {
      head_.reset();
    }
  }
  return original - amount;
}

std::unique_ptr<folly::IOBuf> IOBufQueue::pop_front() {
  auto guard = updateGuard();
  if (!head_) {
    return nullptr;
  }
  chainLength_ -= head_->length();
  std::unique_ptr<folly::IOBuf> retBuf = std::move(head_);
  head_ = retBuf->pop();
  return retBuf;
}

void IOBufQueue::clearAndTryReuseLargestBuffer() {
  auto guard = updateGuard();
  std::unique_ptr<folly::IOBuf> best;
  while (head_) {
    auto buf = std::exchange(head_, head_->pop());
    if (!buf->isSharedOne() &&
        (best == nullptr || buf->capacity() > best->capacity())) {
      best = std::move(buf);
    }
  }
  if (best != nullptr) {
    best->clear();
    head_ = std::move(best);
  }
  chainLength_ = 0;
}

void IOBufQueue::appendToString(std::string& out) const {
  if (!head_) {
    return;
  }
  auto len = options_.cacheChainLength
      ? chainLength_ + (cachePtr_->cachedRange.first - tailStart_)
      : head_->computeChainDataLength() +
          (cachePtr_->cachedRange.first - tailStart_);
  out.reserve(out.size() + len);

  for (auto range : *head_) {
    out.append(reinterpret_cast<const char*>(range.data()), range.size());
  }

  if (tailStart_ != cachePtr_->cachedRange.first) {
    out.append(
        reinterpret_cast<const char*>(tailStart_),
        cachePtr_->cachedRange.first - tailStart_);
  }
}

void IOBufQueue::gather(std::size_t maxLength) {
  auto guard = updateGuard();
  if (head_ != nullptr) {
    head_->gather(maxLength);
  }
}

} // namespace folly
