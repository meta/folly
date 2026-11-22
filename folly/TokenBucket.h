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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include <folly/ConstexprMath.h>
#include <folly/Likely.h>
#include <folly/Optional.h>
#include <folly/concurrency/CacheLocality.h>

namespace folly {

struct TokenBucketPolicyDefault {
  using align =
      std::integral_constant<size_t, hardware_destructive_interference_size>;

  template <typename T>
  using atom = std::atomic<T>;

  using clock = std::chrono::steady_clock;

  using concurrent = std::true_type;
};

/**
 * Thread-safe (atomic) token bucket primitive.
 *
 * This primitive can be used to implement a token bucket
 * (http://en.wikipedia.org/wiki/Token_bucket).  It handles
 * the storage of the state in an atomic way, and presents
 * an interface dealing with tokens, rate, burstSize and time.
 *
 * This primitive records the last time it was updated. This allows the
 * token bucket to add tokens "just in time" when tokens are requested.
 *
 * @tparam Policy A policy.
 */
template <typename Policy = TokenBucketPolicyDefault>
class TokenBucketStorage {
  template <typename T>
  using Atom = typename Policy::template atom<T>;
  using Align = typename Policy::align;
  using Clock = typename Policy::clock; // do we need clock here?
  using Concurrent = typename Policy::concurrent;

  static_assert(Clock::is_steady, "clock must be steady"); // do we need clock?

 public:
  /**
   * Constructor.
   *
   * @param zeroTime Initial time at which to consider the token bucket
   *                 starting to fill. Defaults to 0, so by default token
   *                 buckets are "empty" after construction.
   */
  explicit TokenBucketStorage(double zeroTime = 0) noexcept
      : zeroTime_(zeroTime) {}

  /**
   * Copy constructor.
   *
   * Thread-safe. (Copy constructors of derived classes may not be thread-safe
   * however.)
   */
  TokenBucketStorage(const TokenBucketStorage& other) noexcept
      : zeroTime_(other.zeroTime_.load(std::memory_order_relaxed)) {}

  /**
   * Copy-assignment operator.
   *
   * Warning: not thread safe for the object being assigned to (including
   * self-assignment). Thread-safe for the other object.
   */
  TokenBucketStorage& operator=(const TokenBucketStorage& other) noexcept {
    zeroTime_.store(other.zeroTime(), std::memory_order_relaxed);
    return *this;
  }

  /**
   * Re-initialize token bucket.
   *
   * Thread-safe.
   *
   * @param zeroTime Initial time at which to consider the token bucket
   *                 starting to fill. Defaults to 0, so by default token
   *                 bucket is reset to "empty".
   */
  void reset(double zeroTime = 0) noexcept {
    zeroTime_.store(zeroTime, std::memory_order_relaxed);
  }

  /**
   * Returns the token balance at specified time (negative if bucket in debt).
   *
   * Thread-safe (but returned value may immediately be outdated).
   */
  double balance(
      double rate, double burstSize, double nowInSeconds) const noexcept {
    assert(rate > 0);
    assert(burstSize > 0);
    double zt = this->zeroTime_.load(std::memory_order_relaxed);
    return std::min((nowInSeconds - zt) * rate, burstSize);
  }

  /**
   * Consume tokens at the given rate/burst/time.
   *
   * Consumption is actually done by the callback function: it's given a
   * reference with the number of available tokens and returns the number
   * consumed.  Typically the return value would be between 0.0 and available,
   * but there are no restrictions.
   *
   * Note: the callback may be called multiple times, so please no side-effects
   */
  template <typename Callback>
  double consume(
      double rate,
      double burstSize,
      double nowInSeconds,
      const Callback& callback) {
    assert(rate > 0);
    assert(burstSize > 0);

    double zeroTimeOld;
    double zeroTimeNew;
    double consumed;
    do {
      zeroTimeOld = zeroTime();
      double tokens = std::min((nowInSeconds - zeroTimeOld) * rate, burstSize);
      consumed = callback(tokens);
      double tokensNew = tokens - consumed;
      if (consumed == 0.0) {
        return consumed;
      }

      zeroTimeNew = nowInSeconds - tokensNew / rate;
    } while (FOLLY_UNLIKELY(
        !compare_exchange_weak_relaxed(zeroTime_, zeroTimeOld, zeroTimeNew)));

    return consumed;
  }

  /**
   * returns the time at which the bucket will have `target` tokens available.
   *
   * Caution: it doesn't make sense to ask about target > burstSize
   *
   * Eg.
   *     // time debt repaid
   *     bucket.timeWhenBucket(rate, 0);
   *
   *     // time bucket is full
   *     bucket.timeWhenBucket(rate, burstSize);
   */

  double timeWhenBucket(double rate, double target) {
    return zeroTime() + target / rate;
  }

  /**
   * Return extra tokens back to the bucket.
   *
   * Thread-safe.
   */
  void returnTokens(double tokensToReturn, double rate) {
    assert(rate > 0);

    returnTokensImpl(tokensToReturn, rate);
  }

 private:
  /**
   * Adjust zeroTime based on rate and tokenCount and return the new value of
   * zeroTime_. Note: Token count can be negative to move the zeroTime_
   * into the future.
   */
  double returnTokensImpl(double tokenCount, double rate) {
    double zeroTimeOld; 
    double zeroTimeNew;
    do {
      zeroTimeOld = zeroTime();
      zeroTimeNew = zeroTimeOld - tokenCount / rate;
    } while (FOLLY_UNLIKELY(
        !compare_exchange_weak_relaxed(zeroTime_, zeroTimeOld, zeroTimeNew)));
    
    return zeroTimeNew;
  }

  static bool compare_exchange_weak_relaxed(

      Atom<double>& atom, double& expected, double zeroTime) {
    if (Concurrent::value) {
      return atom.compare_exchange_weak(
          expected, zeroTime, std::memory_order_relaxed);
    } else {
      return atom.store(zeroTime, std::memory_order_relaxed), true;
    }
  }

  double zeroTime() const {
    return this->zeroTime_.load(std::memory_order_relaxed);
  }

  static constexpr size_t AlignZeroTime =
      constexpr_max(Align::value, alignof(Atom<double>));
  alignas(AlignZeroTime) Atom<double> zeroTime_;
};

/**
 * Thread-safe (atomic) token bucket implementation.
 *
 * A token bucket (http://en.wikipedia.org/wiki/Token_bucket) models a stream
 * of events with an average rate and some amount of burstiness. The canonical
 * example is a packet switched network: the network can accept some number of
 * bytes per second and the bytes come in finite packets (bursts). A token
 * bucket stores up to a fixed number of tokens (the burst size). Some number
 * of tokens are removed when an event occurs. The tokens are replenished at a
 * fixed rate. Failure to allocate tokens implies resource is unavailable and
 * caller needs to implement its own retry mechanism. For simple cases where
 * caller is okay with a FIFO starvation-free scheduling behavior, there are
 * also APIs to 'borrow' from the future effectively assigning a start time to
 * the caller when it should proceed with using the resource. It is also
 * possible to 'return' previously allocated tokens to make them available to
 * other users. Returns in excess of burstSize are considered expired and
 * will not be available to later callers.
 *
 * This implementation records the last time it was updated. This allows the
 * token bucket to add tokens "just in time" when tokens are requested.
 *
 * The "dynamic" base variant allows the token generation rate and maximum
 * burst size to change with every token consumption.
 *
 * @tparam Policy A policy.
 */
template <typename Policy = TokenBucketPolicyDefault>
class BasicDynamicTokenBucket {
  template <typename T>
  using Atom = typename Policy::template atom<T>;
  using Align = typename Policy::align;
  using Clock = typename Policy::clock;
  using Concurrent = typename Policy::concurrent;

  static_assert(Clock::is_steady, "clock must be steady");

 public:
  /**
   * Constructor.
   *
   * @param zeroTime Initial time at which to consider the token bucket
   *                 starting to fill. Defaults to 0, so by default token
   *                 buckets are "empty" after construction.
   */
  explicit BasicDynamicTokenBucket(double zeroTime = 0) noexcept
      : bucket_(zeroTime) {}

  /**
   * Copy constructor and copy assignment operator.
   *
   * Thread-safe. (Copy constructors of derived classes may not be thread-safe
   * however.)
   */
  BasicDynamicTokenBucket(const BasicDynamicTokenBucket& other) noexcept =
      default;
  BasicDynamicTokenBucket& operator=(
      const BasicDynamicTokenBucket& other) noexcept = default;

  /**
   * Re-initialize token bucket.
   *
   * Thread-safe.
   *
   * @param zeroTime Initial time at which to consider the token bucket
   *                 starting to fill. Defaults to 0, so by default token
   *                 bucket is reset to "empty".
   */
  void reset(double zeroTime = 0) noexcept { bucket_.reset(zeroTime); }

  /**
   * Returns the current time in seconds since Epoch.
   */
  static double defaultClockNow() noexcept {
    auto const now = Clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
  }

  /**
   * Attempts to consume some number of tokens. Tokens are first added to the
   * bucket based on the time elapsed since the last attempt to consume tokens.
   * Note: Attempts to consume more tokens than the burst size will always
   * fail.
   *
   * Thread-safe.
   *
   * @param toConsume The number of tokens to consume.
   * @param rate Number of tokens to generate per second.
   * @param burstSize Maximum burst size. Must be greater than 0.
   * @param nowInSeconds Current time in seconds. Should be monotonically
   *                     increasing from the nowInSeconds specified in
   *                     this token bucket's constructor.
   * @return True if the rate limit check passed, false otherwise.
   */
  bool consume(
      double toConsume,
      double rate,
      double burstSize,
      double nowInSeconds = defaultClockNow()) {
    assert(rate > 0);
    assert(burstSize > 0);

    if (bucket_.balance(rate, burstSize, nowInSeconds) < 0.0) {
      return 0;
    }

    double consumed = bucket_.consume(
        rate, burstSize, nowInSeconds, [toConsume](double available) {
          return available < toConsume ? 0.0 : toConsume;
        });

    assert(consumed == toConsume || consumed == 0.0);
    return consumed == toConsume;
  }

  /**
   * Similar to consume, but always consumes some number of tokens.  If the
   * bucket contains enough tokens - consumes toConsume tokens.  Otherwise the
   * bucket is drained.
   *
   * Thread-safe.
   *
   * @param toConsume The number of tokens to consume.
   * @param rate Number of tokens to generate per second.
   * @param burstSize Maximum burst size. Must be greater than 0.
   * @param nowInSeconds Current time in seconds. Should be monotonically
   *                     increasing from the nowInSeconds specified in
   *                     this token bucket's constructor.
   * @return number of tokens that were consumed.
   */
  double consumeOrDrain(
      double toConsume,
      double rate,
      double burstSize,
      double nowInSeconds = defaultClockNow()) {
    assert(rate > 0);
    assert(burstSize > 0);

    if (bucket_.balance(rate, burstSize, nowInSeconds) <= 0.0) {
      return 0;
    }

    double consumed = bucket_.consume(
        rate, burstSize, nowInSeconds, [toConsume](double available) {
          return constexpr_min(available, toConsume);
        });
    return consumed;
  }

  /**
   * Return extra tokens back to the bucket.
   *
   * Thread-safe.
   */
  void returnTokens(double tokensToReturn, double rate) {
    assert(rate > 0);
    assert(tokensToReturn > 0);

    bucket_.returnTokens(tokensToReturn, rate);
  }

  /**
   * Like consumeOrDrain but the call will always satisfy the asked for count.
   * It does so by borrowing tokens from the future if the currently available
   * count isn't sufficient.
   *
   * Returns a folly::Optional<double>. The optional wont be set if the request
   * cannot be satisfied: only case is when it is larger than burstSize. The
   * value of the optional is a double indicating the time in seconds that the
   * caller needs to wait at which the reservation becomes valid. The caller
   * could simply sleep for the returned duration to smooth out the allocation
   * to match the rate limiter or do some other computation in the meantime. In
   * any case, any regular consume or consumeOrDrain calls will fail to allocate
   * any tokens until the future time is reached.
   *
   * Note: It is assumed the caller will not ask for a very large count nor use
   * it immediately (if not waiting inline) as that would break the burst
   * prevention the limiter is meant to be used for.
   *
   * Thread-safe.
   */
  Optional<double> consumeWithBorrowNonBlocking(
      double toConsume,
      double rate,
      double burstSize,
      double nowInSeconds = defaultClockNow()) {
    assert(rate > 0);
    assert(burstSize > 0);

    if (burstSize < toConsume) {
      return folly::none;
    }

    while (toConsume > 0) {
      double consumed =
          consumeOrDrain(toConsume, rate, burstSize, nowInSeconds);
      if (consumed > 0) {
        toConsume -= consumed;
      } else {
        bucket_.returnTokens(-toConsume, rate);
        double debtPaid = bucket_.timeWhenBucket(rate, 0);
        double napTime = std::max(0.0, debtPaid - nowInSeconds);
        return napTime;
      }
    }
    return 0;
  }

  /**
   * Convenience wrapper around non-blocking borrow to sleep inline until
   * reservation is valid.
   */
  bool consumeWithBorrowAndWait(
      double toConsume,
      double rate,
      double burstSize,
      double nowInSeconds = defaultClockNow()) {
    auto res =
        consumeWithBorrowNonBlocking(toConsume, rate, burstSize, nowInSeconds);
    if (res.value_or(0) > 0) {
      const auto napUSec = static_cast<int64_t>(res.value() * 1000000);
      std::this_thread::sleep_for(std::chrono::microseconds(napUSec));
    }
    return res.has_value();
  }

  /**
   * Returns the tokens available at specified time (zero if in debt).
   *
   * Use balance() to get the balance of tokens.
   *
   * Thread-safe (but returned value may immediately be outdated).
   */
  double available(
      double rate,
      double burstSize,
      double nowInSeconds = defaultClockNow()) const noexcept {
    return std::max(0.0, balance(rate, burstSize, nowInSeconds));
  }

  /**
   * Returns the token balance at specified time (negative if bucket in debt).
   *
   * Thread-safe (but returned value may immediately be outdated).
   */
  double balance(
      double rate,
      double burstSize,
      double nowInSeconds = defaultClockNow()) const noexcept {
    return bucket_.balance(rate, burstSize, nowInSeconds);
  }

 private:
  TokenBucketStorage<Policy> bucket_;
};

/**
 * Specialization of BasicDynamicTokenBucket with a fixed token
 * generation rate and a fixed maximum burst size.
 */
template <typename Policy = TokenBucketPolicyDefault>
class BasicTokenBucket {
 private:
  using Impl = BasicDynamicTokenBucket<Policy>;

 public:
  /**
   * Construct a token bucket with a specific maximum rate and burst size.
   *
   * @param genRate Number of tokens to generate per second.
   * @param burstSize Maximum burst size. Must be greater than 0.
   * @param zeroTime Initial time at which to consider the token bucket
   *                 starting to fill. Defaults to 0, so by default token
   *                 bucket is "empty" after construction.
   */
  BasicTokenBucket(
      double genRate, double burstSize, double zeroTime = 0) noexcept
      : tokenBucket_(zeroTime), rate_(genRate), burstSize_(burstSize) {
    assert(rate_ > 0);
    assert(burstSize_ > 0);
  }

  /**
   * Copy constructor.
   *
   * Warning: not thread safe!
   */
  BasicTokenBucket(const BasicTokenBucket& other) noexcept = default;

  /**
   * Copy-assignment operator.
   *
   * Warning: not thread safe!
   */
  BasicTokenBucket& operator=(const BasicTokenBucket& other) noexcept = default;

  /**
   * Returns the current time in seconds since Epoch.
   */
  static double defaultClockNow() noexcept(noexcept(Impl::defaultClockNow())) {
    return Impl::defaultClockNow();
  }

  /**
   * Change rate and burst size.
   *
   * Warning: not thread safe!
   *
   * @param genRate Number of tokens to generate per second.
   * @param burstSize Maximum burst size. Must be greater than 0.
   * @param nowInSeconds Current time in seconds. Should be monotonically
   *                     increasing from the nowInSeconds specified in
   *                     this token bucket's constructor.
   */
  void reset(
      double genRate,
      double burstSize,
      double nowInSeconds = defaultClockNow()) noexcept {
    assert(genRate > 0);
    assert(burstSize > 0);
    const double availTokens = available(nowInSeconds);
    rate_ = genRate;
    burstSize_ = burstSize;
    setCapacity(availTokens, nowInSeconds);
  }

  /**
   * Change number of tokens in bucket.
   *
   * Warning: not thread safe!
   *
   * @param tokens Desired number of tokens in bucket after the call.
   * @param nowInSeconds Current time in seconds. Should be monotonically
   *                     increasing from the nowInSeconds specified in
   *                     this token bucket's constructor.
   */
  void setCapacity(double tokens, double nowInSeconds) noexcept {
    tokenBucket_.reset(nowInSeconds - tokens / rate_);
  }

  /**
   * Attempts to consume some number of tokens. Tokens are first added to the
   * bucket based on the time elapsed since the last attempt to consume tokens.
   * Note: Attempts to consume more tokens than the burst size will always
   * fail.
   *
   * Thread-safe.
   *
   * @param toConsume The number of tokens to consume.
   * @param nowInSeconds Current time in seconds. Should be monotonically
   *                     increasing from the nowInSeconds specified in
   *                     this token bucket's constructor.
   * @return True if the rate limit check passed, false otherwise.
   */
  bool consume(double toConsume, double nowInSeconds = defaultClockNow()) {
    return tokenBucket_.consume(toConsume, rate_, burstSize_, nowInSeconds);
  }

  /**
   * Similar to consume, but always consumes some number of tokens.  If the
   * bucket contains enough tokens - consumes toConsume tokens.  Otherwise the
   * bucket is drained.
   *
   * Thread-safe.
   *
   * @param toConsume The number of tokens to consume.
   * @param nowInSeconds Current time in seconds. Should be monotonically
   *                     increasing from the nowInSeconds specified in
   *                     this token bucket's constructor.
   * @return number of tokens that were consumed.
   */
  double consumeOrDrain(
      double toConsume, double nowInSeconds = defaultClockNow()) {
    return tokenBucket_.consumeOrDrain(
        toConsume, rate_, burstSize_, nowInSeconds);
  }

  /**
   * Returns extra token back to the bucket.  Cannot be negative.
   * For negative tokens, setCapacity() can be used
   */
  void returnTokens(double tokensToReturn) {
    return tokenBucket_.returnTokens(tokensToReturn, rate_);
  }

  /**
   * Reserve tokens and return time to wait for in order for the reservation to
   * be compatible with the bucket configuration.
   */
  Optional<double> consumeWithBorrowNonBlocking(
      double toConsume, double nowInSeconds = defaultClockNow()) {
    return tokenBucket_.consumeWithBorrowNonBlocking(
        toConsume, rate_, burstSize_, nowInSeconds);
  }

  /**
   * Reserve tokens. Blocks if need be until reservation is satisfied.
   */
  bool consumeWithBorrowAndWait(
      double toConsume, double nowInSeconds = defaultClockNow()) {
    return tokenBucket_.consumeWithBorrowAndWait(
        toConsume, rate_, burstSize_, nowInSeconds);
  }

  /**
   * Returns the tokens available at specified time (zero if in debt).
   *
   * Use balance() to get the balance of tokens.
   *
   * Thread-safe (but returned value may immediately be outdated).
   */
  double available(double nowInSeconds = defaultClockNow()) const noexcept {
    return std::max(0.0, balance(nowInSeconds));
  }

  /**
   * Returns the token balance at specified time (negative if bucket in debt).
   *
   * Thread-safe (but returned value may immediately be outdated).
   */
  double balance(double nowInSeconds = defaultClockNow()) const noexcept {
    return tokenBucket_.balance(rate_, burstSize_, nowInSeconds);
  }

  /**
   * Returns the number of tokens generated per second.
   *
   * Thread-safe (but returned value may immediately be outdated).
   */
  double rate() const noexcept { return rate_; }

  /**
   * Returns the maximum burst size.
   *
   * Thread-safe (but returned value may immediately be outdated).
   */
  double burst() const noexcept { return burstSize_; }

 private:
  Impl tokenBucket_;
  double rate_;
  double burstSize_;
};

using TokenBucket = BasicTokenBucket<>;
using DynamicTokenBucket = BasicDynamicTokenBucket<>;

} // namespace folly
