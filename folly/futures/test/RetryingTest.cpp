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

#include <folly/futures/Retrying.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include <folly/futures/test/TestExecutor.h>
#include <folly/portability/GTest.h>
#include <folly/portability/SysResource.h>

using namespace std;
using namespace std::chrono;
using namespace folly;

// Runs func up to max_tries times, expects that every run takes at least
// min_duration and that some run takes at most max_duration.
template <typename D, typename F>
void multiAttemptExpectDurationWithin(
    size_t max_tries, D min_duration, D max_duration, const F& func) {
  auto shortest = D::max();
  for (size_t i = 0; i < max_tries && shortest > max_duration; ++i) {
    auto start = steady_clock::now();
    func();
    auto duration = duration_cast<D>(steady_clock::now() - start);
    EXPECT_GE(duration.count(), min_duration.count());
    shortest = std::min(shortest, duration);
  }
  EXPECT_LE(shortest.count(), max_duration.count());
}

TEST(RetryingTest, hasOpCall) {
  using ew = exception_wrapper;
  auto policy_raw = [](size_t n, const ew&) { return n < 3; };
  auto policy_fut = [](size_t n, const ew&) { return makeFuture(n < 3); };
  auto policy_semi_fut = [](size_t n, const ew&) {
    return makeSemiFuture(n < 3);
  };
  using namespace futures::detail;
  EXPECT_TRUE(retrying_policy_traits<decltype(policy_raw)>::is_raw::value);
  EXPECT_TRUE(retrying_policy_traits<decltype(policy_fut)>::is_fut::value);
  EXPECT_TRUE(
      retrying_policy_traits<decltype(policy_semi_fut)>::is_semi_fut::value);
}

TEST(RetryingTest, basic) {
  auto r =
      futures::retrying(
          [](size_t n, const exception_wrapper&) { return n < 3; },
          [](size_t n) {
            return n < 2
                ? makeFuture<size_t>(runtime_error("ha"))
                : makeFuture(n);
          })
          .wait();
  EXPECT_EQ(2, r.value());
}

TEST(RetryingTest, basicUnsafe) {
  auto r =
      futures::retryingUnsafe(
          [](size_t n, const exception_wrapper&) { return n < 3; },
          [](size_t n) {
            return n < 2
                ? makeFuture<size_t>(runtime_error("ha"))
                : makeFuture(n);
          })
          .wait();
  EXPECT_EQ(2, r.value());
}

TEST(RetryingTest, futureFactoryThrows) {
  struct ReturnedException : exception {};
  struct ThrownException : exception {};
  auto result =
      futures::retrying(
          [](size_t n, const exception_wrapper&) { return n < 2; },
          [](size_t n) {
            switch (n) {
              case 0:
                return makeFuture<size_t>(
                    make_exception_wrapper<ReturnedException>());
              case 1:
                throw ThrownException();
              default:
                return makeFuture(n);
            }
          })
          .wait()
          .result();
  EXPECT_THROW(result.throwUnlessValue(), ThrownException);
}

TEST(RetryingTest, futureFactoryThrowsUnsafe) {
  struct ReturnedException : exception {};
  struct ThrownException : exception {};
  auto result =
      futures::retryingUnsafe(
          [](size_t n, const exception_wrapper&) { return n < 2; },
          [](size_t n) {
            switch (n) {
              case 0:
                return makeFuture<size_t>(
                    make_exception_wrapper<ReturnedException>());
              case 1:
                throw ThrownException();
              default:
                return makeFuture(n);
            }
          })
          .wait()
          .result();
  EXPECT_THROW(result.throwUnlessValue(), ThrownException);
}

TEST(RetryingTest, policyThrows) {
  struct eggs : exception {};
  auto r = futures::retrying(
      [](size_t, exception_wrapper) -> bool { throw eggs(); },
      [](size_t) -> Future<size_t> { throw std::runtime_error("ha"); });
  EXPECT_THROW(std::move(r).get(), eggs);
}

TEST(RetryingTest, policyThrowsUnsafe) {
  struct eggs : exception {};
  auto r = futures::retryingUnsafe(
      [](size_t, exception_wrapper) -> bool { throw eggs(); },
      [](size_t) -> Future<size_t> { throw std::runtime_error("ha"); });
  EXPECT_THROW(std::move(r).get(), eggs);
}

TEST(RetryingTest, policyFuture) {
  atomic<size_t> sleeps{0};
  auto r =
      futures::retrying(
          [&](size_t n, const exception_wrapper&) {
            return n < 3
                ? makeFuture(++sleeps).thenValue([](auto&&) { return true; })
                : makeFuture(false);
          },
          [](size_t n) {
            return n < 2
                ? makeFuture<size_t>(runtime_error("ha"))
                : makeFuture(n);
          })
          .wait();
  EXPECT_EQ(2, r.value());
  EXPECT_EQ(2, sleeps);
}

TEST(RetryingTest, policyFutureUnsafe) {
  atomic<size_t> sleeps{0};
  auto r =
      futures::retryingUnsafe(
          [&](size_t n, const exception_wrapper&) {
            return n < 3
                ? makeFuture(++sleeps).thenValue([](auto&&) { return true; })
                : makeFuture(false);
          },
          [](size_t n) {
            return n < 2
                ? makeFuture<size_t>(runtime_error("ha"))
                : makeFuture(n);
          })
          .wait();
  EXPECT_EQ(2, r.value());
  EXPECT_EQ(2, sleeps);
}

TEST(RetryingTest, policySemiFuture) {
  atomic<size_t> sleeps{0};
  auto r =
      futures::retrying(
          [&](size_t n, const exception_wrapper&) {
            return n < 3 ? makeSemiFuture(++sleeps).deferValue([](auto&&) {
              return true;
            })
                         : makeSemiFuture(false);
          },
          [](size_t n) {
            return n < 2
                ? makeFuture<size_t>(runtime_error("ha"))
                : makeFuture(n);
          })
          .wait();
  EXPECT_EQ(2, r.value());
  EXPECT_EQ(2, sleeps);
}

TEST(RetryingTest, policyBasic) {
  auto r =
      futures::retrying(futures::retryingPolicyBasic(3), [](size_t n) {
        return n < 2 ? makeFuture<size_t>(runtime_error("ha")) : makeFuture(n);
      }).wait();
  EXPECT_EQ(2, r.value());
}

TEST(RetryingTest, policyBasicUnsafe) {
  auto r =
      futures::retryingUnsafe(futures::retryingPolicyBasic(3), [](size_t n) {
        return n < 2 ? makeFuture<size_t>(runtime_error("ha")) : makeFuture(n);
      }).wait();
  EXPECT_EQ(2, r.value());
}

TEST(RetryingTest, semifuturePolicyBasic) {
  auto r = futures::retrying(futures::retryingPolicyBasic(3), [](size_t n) {
             return n < 2
                 ? makeSemiFuture<size_t>(runtime_error("ha"))
                 : makeSemiFuture(n);
           }).wait();
  EXPECT_EQ(2, r.value());
}

TEST(RetryingTest, policyCappedJitteredExponentialBackoff) {
  multiAttemptExpectDurationWithin(5, milliseconds(200), milliseconds(600), [] {
    using ms = milliseconds;
    auto r =
        futures::retrying(
            futures::retryingPolicyCappedJitteredExponentialBackoff(
                3,
                ms(100),
                ms(1500),
                0.1,
                mt19937_64(0),
                [](size_t, const exception_wrapper&) { return true; }),
            [](size_t n) {
              return n < 2
                  ? makeFuture<size_t>(runtime_error("ha"))
                  : makeFuture(n);
            })
            .wait();
    EXPECT_EQ(2, r.value());
  });
}

TEST(RetryingTest, policyCappedJitteredExponentialBackoffUnsafe) {
  multiAttemptExpectDurationWithin(5, milliseconds(200), milliseconds(600), [] {
    using ms = milliseconds;
    auto r =
        futures::retryingUnsafe(
            futures::retryingPolicyCappedJitteredExponentialBackoff(
                3,
                ms(100),
                ms(1500),
                0.1,
                mt19937_64(0),
                [](size_t, const exception_wrapper&) { return true; }),
            [](size_t n) {
              return n < 2
                  ? makeFuture<size_t>(runtime_error("ha"))
                  : makeFuture(n);
            })
            .wait();
    EXPECT_EQ(2, r.value());
  });
}

TEST(RetryingTest, policyCappedJitteredExponentialBackoffManyRetries) {
  using namespace futures::detail;
  mt19937_64 rng(0);
  Duration min_backoff(1);

  Duration max_backoff(10000000);
  Duration backoff = retryingJitteredExponentialBackoffDur(
      80, min_backoff, max_backoff, 0, rng);
  EXPECT_EQ(backoff.count(), max_backoff.count());

  max_backoff = Duration(std::numeric_limits<int64_t>::max());
  backoff = retryingJitteredExponentialBackoffDur(
      63, min_backoff, max_backoff, 0, rng);
  EXPECT_LT(backoff.count(), max_backoff.count());

  max_backoff = Duration(std::numeric_limits<int64_t>::max());
  backoff = retryingJitteredExponentialBackoffDur(
      64, min_backoff, max_backoff, 0, rng);
  EXPECT_EQ(backoff.count(), max_backoff.count());
}

TEST(RetryingTest, policyCappedJitteredExponentialBackoffMinZero) {
  using namespace futures::detail;
  mt19937_64 rng(0);

  Duration min_backoff(0);
  Duration max_backoff(2000);

  EXPECT_EQ(
      retryingJitteredExponentialBackoffDur(5, min_backoff, max_backoff, 0, rng)
          .count(),
      0);

  EXPECT_EQ(
      retryingJitteredExponentialBackoffDur(5, min_backoff, max_backoff, 1, rng)
          .count(),
      0);

  EXPECT_EQ(
      retryingJitteredExponentialBackoffDur(
          1025, min_backoff, max_backoff, 0, rng)
          .count(),
      0);
}

TEST(RetryingTest, policySleepDefaults) {
  multiAttemptExpectDurationWithin(5, milliseconds(200), milliseconds(600), [] {
    //  To ensure that this compiles with default params.
    using ms = milliseconds;
    auto r =
        futures::retrying(
            futures::retryingPolicyCappedJitteredExponentialBackoff(
                3, ms(100), ms(1500), 0.1),
            [](size_t n) {
              return n < 2
                  ? makeFuture<size_t>(runtime_error("ha"))
                  : makeFuture(n);
            })
            .wait();
    EXPECT_EQ(2, r.value());
  });
}

TEST(RetryingTest, largeRetries) {
#ifndef _WIN32
  rlimit oldMemLimit;
  PCHECK(getrlimit(RLIMIT_AS, &oldMemLimit) == 0);

  rlimit newMemLimit;
  newMemLimit.rlim_cur =
      std::min(static_cast<rlim_t>(1UL << 30), oldMemLimit.rlim_max);
  newMemLimit.rlim_max = oldMemLimit.rlim_max;
  auto const lowered = // sanitizers reserve outside of the rlimit
      !folly::kIsSanitize && setrlimit(RLIMIT_AS, &newMemLimit) != 0;
  SCOPE_EXIT {
    PCHECK(!lowered || setrlimit(RLIMIT_AS, &oldMemLimit) == 0);
  };
#endif

  TestExecutor executor(4);
  // size of implicit promise is at least the size of the return.
  using LargeReturn = array<uint64_t, 16000>;
  auto func = [&executor](size_t retryNum) -> Future<LargeReturn> {
    return via(&executor).thenValue([retryNum](auto&&) {
      return retryNum < 10000
          ? makeFuture<LargeReturn>(
                make_exception_wrapper<std::runtime_error>("keep trying"))
          : makeFuture<LargeReturn>(LargeReturn());
    });
  };

  vector<SemiFuture<LargeReturn>> futures;
  for (auto idx = 0; idx < 40; ++idx) {
    futures.emplace_back(
        futures::retrying(
            [&executor](size_t, const exception_wrapper&) {
              return via(&executor).thenValue([](auto&&) { return true; });
            },
            func));
  }

  // 40 * 10,000 * 16,000B > 1GB; we should avoid OOM

  for (auto& f : futures) {
    f.wait();
    EXPECT_TRUE(f.hasValue());
  }
}

TEST(RetryingTest, retryingJitteredExponentialBackoffDur) {
  mt19937_64 rng(0);

  auto backoffMin = milliseconds(100);
  auto backoffMax = milliseconds(1000);

  EXPECT_EQ(
      100,
      futures::detail::retryingJitteredExponentialBackoffDur(
          1, backoffMin, backoffMax, 0, rng)
          .count());

  EXPECT_EQ(
      200,
      futures::detail::retryingJitteredExponentialBackoffDur(
          2, backoffMin, backoffMax, 0, rng)
          .count());

  EXPECT_EQ(
      400,
      futures::detail::retryingJitteredExponentialBackoffDur(
          3, backoffMin, backoffMax, 0, rng)
          .count());

  EXPECT_EQ(
      800,
      futures::detail::retryingJitteredExponentialBackoffDur(
          4, backoffMin, backoffMax, 0, rng)
          .count());

  EXPECT_EQ(
      1000,
      futures::detail::retryingJitteredExponentialBackoffDur(
          5, backoffMin, backoffMax, 0, rng)
          .count());

  // Invalid usage: backoffMin > backoffMax
  backoffMax = milliseconds(0);

  EXPECT_EQ(
      100,
      futures::detail::retryingJitteredExponentialBackoffDur(
          1, backoffMin, backoffMax, 0, rng)
          .count());

  EXPECT_EQ(
      100,
      futures::detail::retryingJitteredExponentialBackoffDur(
          1000, backoffMin, backoffMax, 0, rng)
          .count());
}

/*
TEST(RetryingTest, policySleepCancel) {
  multiAttemptExpectDurationWithin(5, milliseconds(0), milliseconds(10), []{
    mt19937_64 rng(0);
    using ms = milliseconds;
    auto r = futures::retrying(
        futures::retryingPolicyCappedJitteredExponentialBackoff(
          5, ms(100), ms(1000), 0.1, rng,
          [](size_t n, const exception_wrapper&) { return true; }),
        [](size_t n) {
            return n < 4
              ? makeFuture<size_t>(runtime_error("ha"))
              : makeFuture(n);
        }
    );
    r.cancel();
    r.wait();
    EXPECT_EQ(2, r.value());
  });
}
*/
