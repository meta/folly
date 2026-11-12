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

#include <folly/synchronization/AtomicUtil.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <utility>

#include <folly/Benchmark.h>
#include <folly/Portability.h>
#include <folly/Utility.h>
#include <folly/portability/GTest.h>
#include <folly/synchronization/AtomicRef.h>
#include <folly/synchronization/RelaxedAtomic.h>

static constexpr auto relaxed = std::memory_order_relaxed;
static constexpr auto consume = std::memory_order_consume;
static constexpr auto acquire = std::memory_order_acquire;
static constexpr auto release = std::memory_order_release;
static constexpr auto acq_rel = std::memory_order_acq_rel;
static constexpr auto seq_cst = std::memory_order_seq_cst;

static_assert(
    std::is_same_v<int, folly::atomic_value_type_t<std::atomic<int>>>);
static_assert(
    std::is_same_v<int, folly::atomic_value_type<std::atomic<int>>::type>);

struct AtomicValueTypeLoadOnly {
  int load() const;
};

static_assert(
    std::is_same_v<int, folly::atomic_value_type_t<AtomicValueTypeLoadOnly>>);

namespace folly {

class MemoryOrderTest : public testing::Test {};

TEST_F(MemoryOrderTest, memory_order_load) {
  EXPECT_EQ(relaxed, memory_order_load(relaxed));
  EXPECT_EQ(relaxed, memory_order_load(release));
  EXPECT_EQ(consume, memory_order_load(consume));
  EXPECT_EQ(acquire, memory_order_load(acquire));
  EXPECT_EQ(acquire, memory_order_load(acq_rel));
  EXPECT_EQ(seq_cst, memory_order_load(seq_cst));
}

TEST_F(MemoryOrderTest, memory_order_store) {
  EXPECT_EQ(relaxed, memory_order_store(relaxed));
  EXPECT_EQ(release, memory_order_store(release));
  EXPECT_EQ(relaxed, memory_order_store(consume));
  EXPECT_EQ(relaxed, memory_order_store(acquire));
  EXPECT_EQ(release, memory_order_store(acq_rel));
  EXPECT_EQ(seq_cst, memory_order_store(seq_cst));
}

class AtomicCompareExchangeSuccTest : public testing::Test {};

TEST_F(AtomicCompareExchangeSuccTest, examples) {
  using detail::atomic_compare_exchange_succ;

  // noop table
  EXPECT_EQ(relaxed, atomic_compare_exchange_succ(false, relaxed, relaxed));
  EXPECT_EQ(consume, atomic_compare_exchange_succ(false, consume, relaxed));
  EXPECT_EQ(acquire, atomic_compare_exchange_succ(false, acquire, relaxed));
  EXPECT_EQ(release, atomic_compare_exchange_succ(false, release, relaxed));
  EXPECT_EQ(acq_rel, atomic_compare_exchange_succ(false, acq_rel, relaxed));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(false, seq_cst, relaxed));
  EXPECT_EQ(relaxed, atomic_compare_exchange_succ(false, relaxed, consume));
  EXPECT_EQ(consume, atomic_compare_exchange_succ(false, consume, consume));
  EXPECT_EQ(acquire, atomic_compare_exchange_succ(false, acquire, consume));
  EXPECT_EQ(release, atomic_compare_exchange_succ(false, release, consume));
  EXPECT_EQ(acq_rel, atomic_compare_exchange_succ(false, acq_rel, consume));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(false, seq_cst, consume));
  EXPECT_EQ(relaxed, atomic_compare_exchange_succ(false, relaxed, acquire));
  EXPECT_EQ(consume, atomic_compare_exchange_succ(false, consume, acquire));
  EXPECT_EQ(acquire, atomic_compare_exchange_succ(false, acquire, acquire));
  EXPECT_EQ(release, atomic_compare_exchange_succ(false, release, acquire));
  EXPECT_EQ(acq_rel, atomic_compare_exchange_succ(false, acq_rel, acquire));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(false, seq_cst, acquire));
  EXPECT_EQ(relaxed, atomic_compare_exchange_succ(false, relaxed, seq_cst));
  EXPECT_EQ(consume, atomic_compare_exchange_succ(false, consume, seq_cst));
  EXPECT_EQ(acquire, atomic_compare_exchange_succ(false, acquire, seq_cst));
  EXPECT_EQ(release, atomic_compare_exchange_succ(false, release, seq_cst));
  EXPECT_EQ(acq_rel, atomic_compare_exchange_succ(false, acq_rel, seq_cst));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(false, seq_cst, seq_cst));

  // xform table
  EXPECT_EQ(relaxed, atomic_compare_exchange_succ(true, relaxed, relaxed));
  EXPECT_EQ(consume, atomic_compare_exchange_succ(true, consume, relaxed));
  EXPECT_EQ(acquire, atomic_compare_exchange_succ(true, acquire, relaxed));
  EXPECT_EQ(release, atomic_compare_exchange_succ(true, release, relaxed));
  EXPECT_EQ(acq_rel, atomic_compare_exchange_succ(true, acq_rel, relaxed));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(true, seq_cst, relaxed));
  EXPECT_EQ(consume, atomic_compare_exchange_succ(true, relaxed, consume));
  EXPECT_EQ(consume, atomic_compare_exchange_succ(true, consume, consume));
  EXPECT_EQ(acquire, atomic_compare_exchange_succ(true, acquire, consume));
  EXPECT_EQ(acq_rel, atomic_compare_exchange_succ(true, release, consume));
  EXPECT_EQ(acq_rel, atomic_compare_exchange_succ(true, acq_rel, consume));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(true, seq_cst, consume));
  EXPECT_EQ(acquire, atomic_compare_exchange_succ(true, relaxed, acquire));
  EXPECT_EQ(acquire, atomic_compare_exchange_succ(true, consume, acquire));
  EXPECT_EQ(acquire, atomic_compare_exchange_succ(true, acquire, acquire));
  EXPECT_EQ(acq_rel, atomic_compare_exchange_succ(true, release, acquire));
  EXPECT_EQ(acq_rel, atomic_compare_exchange_succ(true, acq_rel, acquire));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(true, seq_cst, acquire));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(true, relaxed, seq_cst));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(true, consume, seq_cst));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(true, acquire, seq_cst));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(true, release, seq_cst));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(true, acq_rel, seq_cst));
  EXPECT_EQ(seq_cst, atomic_compare_exchange_succ(true, seq_cst, seq_cst));

  // properties
  for (auto succ : {relaxed, consume, acquire, release, acq_rel, seq_cst}) {
    SCOPED_TRACE(static_cast<int>(succ));
    for (auto fail : {relaxed, consume, acquire, seq_cst}) {
      SCOPED_TRACE(static_cast<int>(fail));
      EXPECT_EQ(succ, atomic_compare_exchange_succ(false, succ, fail));
      auto const sfix = atomic_compare_exchange_succ(true, succ, fail);
      EXPECT_GE(sfix, succ);
      EXPECT_GE(sfix, fail);
      EXPECT_TRUE(fail != relaxed || sfix == succ);
      EXPECT_TRUE(fail == relaxed || succ != release || sfix != release);
    }
  }
}

namespace {

template <typename L>
struct with_order : private L {
  std::memory_order o_;
  explicit with_order(std::memory_order o, L i) noexcept : L{i}, o_{o} {}
  template <typename... A>
  constexpr decltype(auto) operator()(A&&... a) const noexcept {
    return L::operator()(static_cast<A&&>(a)..., o_);
  }
};

template <typename>
struct atomic_ref_;

template <typename Integer>
struct atomic_ref_<std::atomic<Integer>> {
  using reference = std::atomic<Integer>&;
  std::atomic<Integer> value_;
  atomic_ref_() noexcept : value_{} {}
  constexpr explicit atomic_ref_(Integer value) noexcept : value_{value} {}
  constexpr operator reference() & { return reference(value_); }
};

template <typename Integer>
struct atomic_ref_<folly::atomic_ref<Integer>> {
  using reference = folly::atomic_ref<Integer>;
  Integer value_;
  atomic_ref_() noexcept : value_{} {}
  constexpr explicit atomic_ref_(Integer value) noexcept : value_{value} {}
  constexpr operator reference() & { return reference(value_); }
};

#if __cpp_lib_atomic_ref >= 201806L
template <typename Integer>
struct atomic_ref_<std::atomic_ref<Integer>> {
  using reference = std::atomic_ref<Integer>;
  Integer value_;
  atomic_ref_() noexcept : value_{} {}
  constexpr explicit atomic_ref_(Integer value) noexcept : value_{value} {}
  constexpr operator reference() & { return reference(value_); }
};
#endif

template <template <typename> class Atom>
struct atomic_ref_of {
  template <typename Integer>
  using apply = Atom<Integer>;
};

template <typename TypeParam, typename Integer, typename Op>
void atomic_fetch_set_basic(Op fetch_set) {
  using raw_ = typename TypeParam::template apply<Integer>;
  using obj_ = atomic_ref_<raw_>;
  using ref_ = typename obj_::reference;
  constexpr auto Size = 8 / (sizeof(Integer) % 16);
  static_assert(Size > 0);

  for (ref_ atomic : std::array<obj_, Size>{}) {
    atomic.store(0b0);
    EXPECT_EQ(fetch_set(atomic, 0), false);
    EXPECT_EQ(fetch_set(atomic, 1), false);
    EXPECT_EQ(atomic.load(), 0b11);
    EXPECT_EQ(fetch_set(atomic, 2), false);
    EXPECT_EQ(atomic.load(), 0b111);
  }

  for (ref_ atomic : std::array<obj_, Size>{}) {
    atomic.store(0b1);
    EXPECT_EQ(fetch_set(atomic, 0), true);
    EXPECT_EQ(fetch_set(atomic, 0), true);
    EXPECT_EQ(fetch_set(atomic, 1), false);
    EXPECT_EQ(atomic.load(), 0b11);
    EXPECT_EQ(fetch_set(atomic, 2), false);
    EXPECT_EQ(atomic.load(), 0b111);
  }

  {
    for (auto i = 0; i < 100000; ++i) {
      // call makeUnpredictable() to ensure that the bit integer does not get
      // optimized away.  This is testing the feasability of this code in
      // situations where bit is not known at compile time and will likely force
      // a register load
      obj_ atomic_{0b0};
      ref_ atomic = atomic_;
      auto&& bit = 0;
      folly::makeUnpredictable(bit);

      EXPECT_EQ(fetch_set(atomic, bit), false);
      EXPECT_EQ(fetch_set(atomic, bit + 1), false);
      EXPECT_EQ(atomic.load(), 0b11);
      EXPECT_EQ(fetch_set(atomic, bit + 2), false);
      EXPECT_EQ(atomic.load(), 0b111);
    }
  }
}

template <typename TypeParam, typename Integer, typename Op>
void atomic_fetch_reset_basic(Op fetch_reset) {
  using raw_ = typename TypeParam::template apply<Integer>;
  using obj_ = atomic_ref_<raw_>;
  using ref_ = typename obj_::reference;
  constexpr auto Size = 8 / (sizeof(Integer) % 16);
  static_assert(Size > 0);

  for (ref_ atomic : std::array<obj_, Size>{}) {
    EXPECT_EQ(fetch_reset(atomic, 0), false);
    EXPECT_EQ(fetch_reset(atomic, 1), false);
    atomic.store(0b11);
    EXPECT_EQ(fetch_reset(atomic, 0), true);
    EXPECT_EQ(fetch_reset(atomic, 1), true);
    EXPECT_EQ(atomic.load(), 0);
  }

  for (ref_ atomic : std::array<obj_, Size>{}) {
    EXPECT_EQ(fetch_reset(atomic, 0), false);
    EXPECT_EQ(fetch_reset(atomic, 1), false);
    atomic.store(0b11);
    EXPECT_EQ(fetch_reset(atomic, 1), true);
    EXPECT_EQ(fetch_reset(atomic, 0), true);
    EXPECT_EQ(atomic.load(), 0);
  }
}

template <typename TypeParam, typename Integer, typename Op>
void atomic_fetch_flip_basic(Op fetch_flip) {
  using raw_ = typename TypeParam::template apply<Integer>;
  using obj_ = atomic_ref_<raw_>;
  using ref_ = typename obj_::reference;
  constexpr auto Size = 8 / (sizeof(Integer) % 16);
  static_assert(Size > 0);

  for (ref_ atomic : std::array<obj_, Size>{}) {
    EXPECT_EQ(fetch_flip(atomic, 0), false);
    EXPECT_EQ(fetch_flip(atomic, 1), false);
    atomic.store(0b11);
    EXPECT_EQ(fetch_flip(atomic, 0), true);
    EXPECT_EQ(fetch_flip(atomic, 1), true);
    EXPECT_EQ(atomic.load(), 0);
  }

  for (ref_ atomic : std::array<obj_, Size>{}) {
    EXPECT_EQ(fetch_flip(atomic, 0), false);
    EXPECT_EQ(fetch_flip(atomic, 1), false);
    atomic.store(0b10);
    EXPECT_EQ(fetch_flip(atomic, 1), true);
    EXPECT_EQ(fetch_flip(atomic, 0), false);
    EXPECT_EQ(atomic.load(), 0b01);
  }
}

template <typename Integer>
class Atomic {
 public:
  using value_type = Integer;

  Integer fetch_or(Integer value, std::memory_order = seq_cst) {
    ++counts.set;
    return std::exchange(integer_, integer_ | value);
  }
  Integer fetch_and(Integer value, std::memory_order = seq_cst) {
    ++counts.reset;
    return std::exchange(integer_, integer_ & value);
  }
  Integer fetch_xor(Integer value, std::memory_order = seq_cst) {
    ++counts.flip;
    return std::exchange(integer_, integer_ ^ value);
  }

  Integer load(std::memory_order = seq_cst) { return integer_; }

  Integer integer_{0};

  struct counts_ {
    size_t set{0};
    size_t reset{0};
    size_t flip{0};
  };
  counts_ counts;
};

template <typename Integer, typename Op>
void atomic_fetch_set_non_std_atomic(Op fetch_set) {
  auto atomic = Atomic<Integer>{};
  auto& sets = atomic.counts.set;
  auto& resets = atomic.counts.reset;
  auto& flips = atomic.counts.flip;

  fetch_set(atomic, 0);
  EXPECT_EQ(sets, 1);
  EXPECT_EQ(resets, 0);
  EXPECT_EQ(flips, 0);
  EXPECT_EQ(atomic.integer_, 0b1);

  fetch_set(atomic, 2);
  EXPECT_EQ(sets, 2);
  EXPECT_EQ(resets, 0);
  EXPECT_EQ(flips, 0);
  EXPECT_EQ(atomic.integer_, 0b101);
}

template <typename Integer, typename Op>
void atomic_fetch_reset_non_std_atomic(Op fetch_reset) {
  auto atomic = Atomic<Integer>{};
  auto& sets = atomic.counts.set;
  auto& resets = atomic.counts.reset;
  auto& flips = atomic.counts.flip;
  atomic.integer_ = 0b111;

  fetch_reset(atomic, 0);
  EXPECT_EQ(sets, 0);
  EXPECT_EQ(resets, 1);
  EXPECT_EQ(flips, 0);
  EXPECT_EQ(atomic.integer_, 0b110);

  fetch_reset(atomic, 2);
  EXPECT_EQ(sets, 0);
  EXPECT_EQ(resets, 2);
  EXPECT_EQ(flips, 0);
  EXPECT_EQ(atomic.integer_, 0b010);
}

template <typename Integer, typename Op>
void atomic_fetch_flip_non_std_atomic(Op fetch_flip) {
  auto atomic = Atomic<Integer>{};
  auto& sets = atomic.counts.set;
  auto& resets = atomic.counts.reset;
  auto& flips = atomic.counts.flip;
  atomic.integer_ = 0b110;

  fetch_flip(atomic, 0);
  EXPECT_EQ(sets, 0);
  EXPECT_EQ(resets, 0);
  EXPECT_EQ(flips, 1);
  EXPECT_EQ(atomic.integer_, 0b111);

  fetch_flip(atomic, 2);
  EXPECT_EQ(sets, 0);
  EXPECT_EQ(resets, 0);
  EXPECT_EQ(flips, 2);
  EXPECT_EQ(atomic.integer_, 0b011);
}
} // namespace

template <typename Param>
class AtomicFetchSetTest : public ::testing::TestWithParam<Param> {};
template <typename Param>
class AtomicFetchResetTest : public ::testing::TestWithParam<Param> {};
template <typename Param>
class AtomicFetchFlipTest : public ::testing::TestWithParam<Param> {};

TYPED_TEST_SUITE_P(AtomicFetchSetTest);
TYPED_TEST_SUITE_P(AtomicFetchResetTest);
TYPED_TEST_SUITE_P(AtomicFetchFlipTest);

TYPED_TEST_P(AtomicFetchSetTest, Basic) {
  auto op = with_order{seq_cst, folly::atomic_fetch_set};

  atomic_fetch_set_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint8_t>(op);
}

TYPED_TEST_P(AtomicFetchResetTest, Basic) {
  auto op = with_order{seq_cst, folly::atomic_fetch_reset};

  atomic_fetch_reset_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint8_t>(op);
}

TYPED_TEST_P(AtomicFetchFlipTest, Basic) {
  auto op = with_order{seq_cst, folly::atomic_fetch_flip};

  atomic_fetch_flip_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint8_t>(op);
}

TYPED_TEST_P(AtomicFetchSetTest, BasicRelaxed) {
  auto op = with_order{relaxed, folly::atomic_fetch_set};

  atomic_fetch_set_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint8_t>(op);
}

TYPED_TEST_P(AtomicFetchResetTest, BasicRelaxed) {
  auto op = with_order{relaxed, folly::atomic_fetch_reset};

  atomic_fetch_reset_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint8_t>(op);
}

TYPED_TEST_P(AtomicFetchFlipTest, BasicRelaxed) {
  auto op = with_order{relaxed, folly::atomic_fetch_flip};

  atomic_fetch_flip_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint8_t>(op);
}

TYPED_TEST_P(AtomicFetchSetTest, EnsureFetchOrUsed) {
  auto op = with_order{seq_cst, folly::atomic_fetch_set};

  atomic_fetch_set_non_std_atomic<std::uint8_t>(op);
  atomic_fetch_set_non_std_atomic<std::uint16_t>(op);
  atomic_fetch_set_non_std_atomic<std::uint32_t>(op);
  atomic_fetch_set_non_std_atomic<std::uint64_t>(op);
}

TYPED_TEST_P(AtomicFetchResetTest, EnsureFetchAndUsed) {
  auto op = with_order{seq_cst, folly::atomic_fetch_reset};

  atomic_fetch_reset_non_std_atomic<std::uint8_t>(op);
  atomic_fetch_reset_non_std_atomic<std::uint16_t>(op);
  atomic_fetch_reset_non_std_atomic<std::uint32_t>(op);
  atomic_fetch_reset_non_std_atomic<std::uint64_t>(op);
}

TYPED_TEST_P(AtomicFetchFlipTest, EnsureFetchXorUsed) {
  auto op = with_order{seq_cst, folly::atomic_fetch_flip};

  atomic_fetch_flip_non_std_atomic<std::uint8_t>(op);
  atomic_fetch_flip_non_std_atomic<std::uint16_t>(op);
  atomic_fetch_flip_non_std_atomic<std::uint32_t>(op);
  atomic_fetch_flip_non_std_atomic<std::uint64_t>(op);
}

TYPED_TEST_P(AtomicFetchSetTest, FetchSetFallback) {
  auto op = with_order{seq_cst, folly::detail::atomic_fetch_set_fallback};

  atomic_fetch_set_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint8_t>(op);

  atomic_fetch_set_non_std_atomic<std::uint8_t>(op);
  atomic_fetch_set_non_std_atomic<std::uint16_t>(op);
  atomic_fetch_set_non_std_atomic<std::uint32_t>(op);
  atomic_fetch_set_non_std_atomic<std::uint64_t>(op);
}

TYPED_TEST_P(AtomicFetchResetTest, FetchResetFallback) {
  auto op = with_order{seq_cst, folly::detail::atomic_fetch_reset_fallback};

  atomic_fetch_reset_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint8_t>(op);

  atomic_fetch_reset_non_std_atomic<std::uint8_t>(op);
  atomic_fetch_reset_non_std_atomic<std::uint16_t>(op);
  atomic_fetch_reset_non_std_atomic<std::uint32_t>(op);
  atomic_fetch_reset_non_std_atomic<std::uint64_t>(op);
}

TYPED_TEST_P(AtomicFetchFlipTest, FetchFlipFallback) {
  auto op = with_order{seq_cst, folly::detail::atomic_fetch_flip_fallback};

  atomic_fetch_flip_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint8_t>(op);

  atomic_fetch_flip_non_std_atomic<std::uint8_t>(op);
  atomic_fetch_flip_non_std_atomic<std::uint16_t>(op);
  atomic_fetch_flip_non_std_atomic<std::uint32_t>(op);
  atomic_fetch_flip_non_std_atomic<std::uint64_t>(op);
}

TYPED_TEST_P(AtomicFetchSetTest, FetchSetDefault) {
  auto op = folly::atomic_fetch_set;

  atomic_fetch_set_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_set_basic<TypeParam, std::uint8_t>(op);

  atomic_fetch_set_non_std_atomic<std::uint8_t>(op);
  atomic_fetch_set_non_std_atomic<std::uint16_t>(op);
  atomic_fetch_set_non_std_atomic<std::uint32_t>(op);
  atomic_fetch_set_non_std_atomic<std::uint64_t>(op);
}

TYPED_TEST_P(AtomicFetchResetTest, FetchResetDefault) {
  auto op = folly::atomic_fetch_reset;

  atomic_fetch_reset_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_reset_basic<TypeParam, std::uint8_t>(op);

  atomic_fetch_reset_non_std_atomic<std::uint8_t>(op);
  atomic_fetch_reset_non_std_atomic<std::uint16_t>(op);
  atomic_fetch_reset_non_std_atomic<std::uint32_t>(op);
  atomic_fetch_reset_non_std_atomic<std::uint64_t>(op);
}

TYPED_TEST_P(AtomicFetchFlipTest, FetchFlipDefault) {
  auto op = folly::atomic_fetch_flip;

  atomic_fetch_flip_basic<TypeParam, std::uint16_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint32_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint64_t>(op);
  atomic_fetch_flip_basic<TypeParam, std::uint8_t>(op);

  atomic_fetch_flip_non_std_atomic<std::uint8_t>(op);
  atomic_fetch_flip_non_std_atomic<std::uint16_t>(op);
  atomic_fetch_flip_non_std_atomic<std::uint32_t>(op);
  atomic_fetch_flip_non_std_atomic<std::uint64_t>(op);
}

REGISTER_TYPED_TEST_SUITE_P(
    AtomicFetchSetTest,
    Basic,
    BasicRelaxed,
    EnsureFetchOrUsed,
    FetchSetFallback,
    FetchSetDefault);

REGISTER_TYPED_TEST_SUITE_P(
    AtomicFetchResetTest,
    Basic,
    BasicRelaxed,
    EnsureFetchAndUsed,
    FetchResetFallback,
    FetchResetDefault);

REGISTER_TYPED_TEST_SUITE_P(
    AtomicFetchFlipTest,
    Basic,
    BasicRelaxed,
    EnsureFetchXorUsed,
    FetchFlipFallback,
    FetchFlipDefault);

INSTANTIATE_TYPED_TEST_SUITE_P(
    StdAtomic, AtomicFetchSetTest, atomic_ref_of<std::atomic>);
INSTANTIATE_TYPED_TEST_SUITE_P(
    FollyAtomicRef, AtomicFetchSetTest, atomic_ref_of<folly::atomic_ref>);
#if __cpp_lib_atomic_ref >= 201806L
INSTANTIATE_TYPED_TEST_SUITE_P(
    StdAtomicRef, AtomicFetchSetTest, atomic_ref_of<std::atomic_ref>);
#endif

INSTANTIATE_TYPED_TEST_SUITE_P(
    StdAtomic, AtomicFetchResetTest, atomic_ref_of<std::atomic>);
INSTANTIATE_TYPED_TEST_SUITE_P(
    FollyAtomicRef, AtomicFetchResetTest, atomic_ref_of<folly::atomic_ref>);
#if __cpp_lib_atomic_ref >= 201806L
INSTANTIATE_TYPED_TEST_SUITE_P(
    StdAtomicRef, AtomicFetchResetTest, atomic_ref_of<std::atomic_ref>);
#endif

INSTANTIATE_TYPED_TEST_SUITE_P(
    StdAtomic, AtomicFetchFlipTest, atomic_ref_of<std::atomic>);
INSTANTIATE_TYPED_TEST_SUITE_P(
    FollyAtomicRef, AtomicFetchFlipTest, atomic_ref_of<folly::atomic_ref>);
#if __cpp_lib_atomic_ref >= 201806L
INSTANTIATE_TYPED_TEST_SUITE_P(
    StdAtomicRef, AtomicFetchFlipTest, atomic_ref_of<std::atomic_ref>);
#endif

struct AtomicFetchBitOpRelaxedAtomicTest : testing::Test {};

TEST_F(AtomicFetchBitOpRelaxedAtomicTest, setResetFlip) {
  relaxed_atomic<unsigned> cell{0b0100};

  EXPECT_FALSE(folly::atomic_fetch_set(cell, 1));
  EXPECT_EQ(0b0110, cell.load());
  EXPECT_TRUE(folly::atomic_fetch_set(cell, 1));
  EXPECT_EQ(0b0110, cell.load());

  EXPECT_TRUE(folly::atomic_fetch_reset(cell, 2));
  EXPECT_EQ(0b0010, cell.load());
  EXPECT_FALSE(folly::atomic_fetch_reset(cell, 2));
  EXPECT_EQ(0b0010, cell.load());

  EXPECT_FALSE(folly::atomic_fetch_flip(cell, 3));
  EXPECT_EQ(0b1010, cell.load());
  EXPECT_TRUE(folly::atomic_fetch_flip(cell, 3));
  EXPECT_EQ(0b0010, cell.load());
}

struct AtomicFetchModifyTest : testing::Test {};

TEST_F(AtomicFetchModifyTest, example) {
  constexpr auto prime255 = 1619;
  constexpr auto op = [=](auto _) { return (_ + 3) % prime255; };
  std::atomic<int> cell{2};
  auto const prev = folly::atomic_fetch_modify(cell, op, relaxed);
  EXPECT_EQ(2, prev);
  EXPECT_EQ(5, cell.load(relaxed));
}

TEST_F(AtomicFetchModifyTest, contention) {
  constexpr auto prime255 = 1619;
  constexpr size_t lg_nthreads = 6;
  constexpr size_t lg_required_contention = 9;
  constexpr auto iterate = [](auto v, size_t c, auto f) {
    while (c--) {
      v = f(v);
    }
    return v;
  };
  constexpr auto op_ = [=](auto _) { return (_ + 3) % prime255; };

  // run concurrent atomic-fetch-modify ops until enough contention is observed,
  // where contention observed is ~ number of times an op is repeated

  std::atomic<int> cell{2};
  std::atomic<size_t> iters{0};
  std::atomic<size_t> calls{0};
  auto const op = [&](auto const _) {
    calls.fetch_add(1, relaxed);
    return op_(_);
  };
  std::vector<std::thread> threads(1ULL << lg_nthreads);
  std::atomic<bool> stop{false};
  for (auto& th : threads) {
    th = std::thread([&] {
      while (!stop.load(relaxed)) {
        iters.fetch_add(1, relaxed); // incr first
        folly::atomic_fetch_modify(cell, op, relaxed);
      }
    });
  }

  constexpr auto required_contention =
      to_signed(1ULL << lg_required_contention);
  auto const contention = [&] {
    auto const c = folly::to_signed(calls.load(relaxed));
    auto const i = folly::to_signed(iters.load(relaxed));
    return c < prime255 || i < prime255 ? 0 : c - i;
  };
  while (contention() < required_contention) {
    ;
  }
  stop.store(true, relaxed);
  for (auto& th : threads) {
    th.join();
  }
  ASSERT_GE(contention(), required_contention);

  // compare the contended result to an expected uncontended result

  EXPECT_EQ(iterate(2, iters.load(relaxed), op_), cell.load(relaxed));
}

//  pin which dispatch branch each atomic-like type takes, since the branches
//  are otherwise indistinguishable from their results
static_assert(atomic_accepts_memory_order_v<std::atomic<int>>);
static_assert(atomic_accepts_memory_order_v<atomic_ref<int>>);
static_assert(!atomic_accepts_memory_order_v<relaxed_atomic<int>>);
static_assert(!atomic_accepts_memory_order_v<relaxed_atomic<double>>);

//  and pin that the overloads taking a memory order drop out for the types
//  which have no memory order to apply
template <typename Fn, typename Atomic, typename Arg>
inline constexpr bool invocable_both_ = false //
    || !std::is_invocable_v<Fn, Atomic&, Arg> //
    || !std::is_invocable_v<Fn, Atomic&, Arg, std::memory_order>;
static_assert(
    !invocable_both_<atomic_fetch_set_fn, std::atomic<unsigned>, int>);
static_assert(
    !invocable_both_<atomic_fetch_reset_fn, atomic_ref<unsigned>, int>);
static_assert(!invocable_both_<atomic_fetch_max_fn, std::atomic<int>, int>);
template <typename Fn, typename Atomic, typename Arg>
inline constexpr bool invocable_nomo_only_ = true //
    && std::is_invocable_v<Fn, Atomic&, Arg> //
    && !std::is_invocable_v<Fn, Atomic&, Arg, std::memory_order>;
static_assert(
    invocable_nomo_only_<atomic_fetch_set_fn, relaxed_atomic<unsigned>, int>);
static_assert(
    invocable_nomo_only_<atomic_fetch_reset_fn, relaxed_atomic<unsigned>, int>);
static_assert(
    invocable_nomo_only_<atomic_fetch_flip_fn, relaxed_atomic<unsigned>, int>);
static_assert(
    invocable_nomo_only_<atomic_fetch_min_fn, relaxed_atomic<int>, int>);
static_assert(
    invocable_nomo_only_<atomic_fetch_max_fn, relaxed_atomic<int>, int>);

TEST_F(AtomicFetchModifyTest, relaxedAtomic) {
  relaxed_atomic<int> cell{2};
  constexpr auto op = [](auto _) { return _ + 3; };

  EXPECT_EQ(2, folly::atomic_fetch_modify(cell, op));
  EXPECT_EQ(5, cell.load());
  EXPECT_EQ(5, folly::atomic_fetch_modify(cell, op));
  EXPECT_EQ(8, cell.load());
}

struct AtomicFetchMinMaxTest : testing::Test {};

struct AtomicFetchMinMaxMember {
  using value_type = int;

  int load(std::memory_order = seq_cst) const { return value; }

  int fetch_min(int arg, std::memory_order order) {
    ++fetchMinCalls;
    lastOrder = order;
    return std::exchange(value, arg < value ? arg : value);
  }

  int fetch_max(int arg, std::memory_order order) {
    ++fetchMaxCalls;
    lastOrder = order;
    return std::exchange(value, value < arg ? arg : value);
  }

  int value{5};
  int fetchMinCalls{0};
  int fetchMaxCalls{0};
  std::memory_order lastOrder{seq_cst};
};

//  as relaxed_atomic does under c++26: members which take no memory order
struct AtomicFetchMinMaxMemberNoOrder {
  using value_type = int;

  int load() const { return value; }

  int fetch_min(int arg) {
    ++fetchMinCalls;
    return std::exchange(value, arg < value ? arg : value);
  }

  int fetch_max(int arg) {
    ++fetchMaxCalls;
    return std::exchange(value, value < arg ? arg : value);
  }

  int value{5};
  int fetchMinCalls{0};
  int fetchMaxCalls{0};
};

TEST_F(AtomicFetchMinMaxTest, min) {
  std::atomic<int> cell{5};

  EXPECT_EQ(5, folly::atomic_fetch_min(cell, 3, relaxed));
  EXPECT_EQ(3, cell.load(relaxed));
  EXPECT_EQ(3, folly::atomic_fetch_min(cell, 7));
  EXPECT_EQ(3, cell.load());
  EXPECT_EQ(3, folly::atomic_fetch_min(cell, 3));
  EXPECT_EQ(3, cell.load());
}

TEST_F(AtomicFetchMinMaxTest, max) {
  std::atomic<int> cell{5};

  EXPECT_EQ(5, folly::atomic_fetch_max(cell, 7, relaxed));
  EXPECT_EQ(7, cell.load(relaxed));
  EXPECT_EQ(7, folly::atomic_fetch_max(cell, 3));
  EXPECT_EQ(7, cell.load());
  EXPECT_EQ(7, folly::atomic_fetch_max(cell, 7));
  EXPECT_EQ(7, cell.load());
}

TEST_F(AtomicFetchMinMaxTest, atomicRef) {
  int value = 5;
  folly::atomic_ref<int> cell{value};

  EXPECT_EQ(5, folly::atomic_fetch_min(cell, 3, relaxed));
  EXPECT_EQ(3, value);
  EXPECT_EQ(3, folly::atomic_fetch_max(cell, 7, relaxed));
  EXPECT_EQ(7, value);
}

TEST_F(AtomicFetchMinMaxTest, member) {
  AtomicFetchMinMaxMember cell;

  EXPECT_EQ(5, folly::atomic_fetch_min(cell, 3, relaxed));
  EXPECT_EQ(3, cell.value);
  EXPECT_EQ(1, cell.fetchMinCalls);
  EXPECT_EQ(relaxed, cell.lastOrder);

  EXPECT_EQ(3, folly::atomic_fetch_max(cell, 7, acquire));
  EXPECT_EQ(7, cell.value);
  EXPECT_EQ(1, cell.fetchMaxCalls);
  EXPECT_EQ(acquire, cell.lastOrder);
}

TEST_F(AtomicFetchMinMaxTest, memberNoOrder) {
  AtomicFetchMinMaxMemberNoOrder cell;

  EXPECT_EQ(5, folly::atomic_fetch_min(cell, 3));
  EXPECT_EQ(3, cell.value);
  EXPECT_EQ(1, cell.fetchMinCalls);

  EXPECT_EQ(3, folly::atomic_fetch_max(cell, 7));
  EXPECT_EQ(7, cell.value);
  EXPECT_EQ(1, cell.fetchMaxCalls);
}

TEST_F(AtomicFetchMinMaxTest, fallback) {
  std::atomic<double> cell{5};

  EXPECT_EQ(5, folly::atomic_fetch_min(cell, 3, relaxed));
  EXPECT_EQ(3, cell.load(relaxed));
  EXPECT_EQ(3, folly::atomic_fetch_max(cell, 7, relaxed));
  EXPECT_EQ(7, cell.load(relaxed));
}

struct AtomicFetchMinMaxCondTest : testing::Test {};

//  counts stores; the elision of the store is the whole point of the cond
//  operations and is otherwise unobservable
struct AtomicFetchCondProbe {
  using value_type = int;

  int load(std::memory_order order) const {
    lastLoadOrder = order;
    return value;
  }

  bool compare_exchange_weak(int&, int desired, std::memory_order) {
    ++stores;
    value = desired;
    return true;
  }

  int value{5};
  int stores{0};
  mutable std::memory_order lastLoadOrder{seq_cst};
};

//  models a competing writer: the first compare-exchange fails and reports a
//  value already raised past the one being proposed, so a loop which re-tests
//  its guard on retry elides the store, while one which does not stores a value
//  it has just been told is stale
struct AtomicFetchCondRaceProbe {
  using value_type = int;

  int load(std::memory_order) const { return value; }

  bool compare_exchange_weak(int& expected, int desired, std::memory_order) {
    ++attempts;
    if (attempts == 1) {
      value = raised;
      expected = raised;
      return false;
    }
    ++stores;
    value = desired;
    return true;
  }

  int value{0};
  int raised{100};
  int attempts{0};
  int stores{0};
};

TEST_F(AtomicFetchMinMaxCondTest, elidesStoreWhenConverged) {
  AtomicFetchCondProbe cell;

  //  already converged: no store, and the previous value is returned
  EXPECT_EQ(5, folly::atomic_fetch_max_cond(cell, 3, relaxed));
  EXPECT_EQ(0, cell.stores);
  EXPECT_EQ(5, folly::atomic_fetch_min_cond(cell, 7, relaxed));
  EXPECT_EQ(0, cell.stores);
  //  equal is converged too, for both directions
  EXPECT_EQ(5, folly::atomic_fetch_max_cond(cell, 5, relaxed));
  EXPECT_EQ(5, folly::atomic_fetch_min_cond(cell, 5, relaxed));
  EXPECT_EQ(0, cell.stores);
  EXPECT_EQ(5, cell.value);

  //  not converged: stores, and still returns the previous value
  EXPECT_EQ(5, folly::atomic_fetch_max_cond(cell, 7, relaxed));
  EXPECT_EQ(1, cell.stores);
  EXPECT_EQ(7, cell.value);
  EXPECT_EQ(7, folly::atomic_fetch_min_cond(cell, 3, relaxed));
  EXPECT_EQ(2, cell.stores);
  EXPECT_EQ(3, cell.value);
}

//  on the elided path the load part of the memory order is still applied, so
//  that a caller passing an acquire order is served even when nothing is stored
TEST_F(AtomicFetchMinMaxCondTest, appliesLoadOrderWhenConverged) {
  AtomicFetchCondProbe cell; // value 5; every call below is converged

  folly::atomic_fetch_max_cond(cell, 3, acquire);
  EXPECT_EQ(acquire, cell.lastLoadOrder);
  folly::atomic_fetch_max_cond(cell, 3, acq_rel);
  EXPECT_EQ(acquire, cell.lastLoadOrder);
  folly::atomic_fetch_max_cond(cell, 3, release);
  EXPECT_EQ(relaxed, cell.lastLoadOrder);
  folly::atomic_fetch_max_cond(cell, 3, seq_cst);
  EXPECT_EQ(seq_cst, cell.lastLoadOrder);

  EXPECT_EQ(0, cell.stores);
}

//  the guard is re-tested after a failed c/x, so convergence reached during the
//  loop elides the store just as convergence seen by the trial load does
TEST_F(AtomicFetchMinMaxCondTest, elidesStoreWhenConvergedOnRetry) {
  AtomicFetchCondRaceProbe cell; // value 0, raised to 100 by the failed c/x

  //  0 < 7 warrants a store, but the c/x fails reporting 100, and 100 is not
  //  below 7, so the retry must elide rather than store either value
  EXPECT_EQ(100, folly::atomic_fetch_max_cond(cell, 7, relaxed));
  EXPECT_EQ(1, cell.attempts);
  EXPECT_EQ(0, cell.stores);
  EXPECT_EQ(100, cell.value);
}

//  the mirror case: when the retry is still not converged, the store proceeds
TEST_F(AtomicFetchMinMaxCondTest, storesWhenStillUnconvergedOnRetry) {
  AtomicFetchCondRaceProbe cell;
  cell.raised = 3; // below the proposed value, so the retry still stores

  EXPECT_EQ(3, folly::atomic_fetch_max_cond(cell, 7, relaxed));
  EXPECT_EQ(2, cell.attempts);
  EXPECT_EQ(1, cell.stores);
  EXPECT_EQ(7, cell.value);
}

//  delegates to the member fast path only when it actually stores
TEST_F(AtomicFetchMinMaxCondTest, member) {
  AtomicFetchMinMaxMember cell;

  EXPECT_EQ(5, folly::atomic_fetch_max_cond(cell, 3, relaxed));
  EXPECT_EQ(0, cell.fetchMaxCalls);
  EXPECT_EQ(5, cell.value);

  EXPECT_EQ(5, folly::atomic_fetch_max_cond(cell, 7, acquire));
  EXPECT_EQ(1, cell.fetchMaxCalls);
  EXPECT_EQ(7, cell.value);
}

//  the store takes the order in full, not merely the store part: it is itself a
//  read-modify-write, and its own load component yields the returned value, so
//  weakening it to memory_order_store would drop the load part from that value
TEST_F(AtomicFetchMinMaxCondTest, appliesFullOrderWhenStoring) {
  auto const store_order_of = [](std::memory_order order) {
    AtomicFetchMinMaxMember cell; // value 5; every call below stores
    folly::atomic_fetch_max_cond(cell, 7, order);
    EXPECT_EQ(1, cell.fetchMaxCalls);
    return cell.lastOrder;
  };

  for (auto const order : {relaxed, acquire, release, acq_rel, seq_cst}) {
    SCOPED_TRACE(static_cast<int>(order));
    EXPECT_EQ(order, store_order_of(order));
  }
}

TEST_F(AtomicFetchMinMaxCondTest, stdAtomic) {
  std::atomic<int> cell{5};

  EXPECT_EQ(5, folly::atomic_fetch_max_cond(cell, 7, relaxed));
  EXPECT_EQ(7, cell.load(relaxed));
  EXPECT_EQ(7, folly::atomic_fetch_max_cond(cell, 3));
  EXPECT_EQ(7, cell.load());
  EXPECT_EQ(7, folly::atomic_fetch_min_cond(cell, 3, relaxed));
  EXPECT_EQ(3, cell.load(relaxed));
}

TEST_F(AtomicFetchMinMaxCondTest, relaxedAtomic) {
  relaxed_atomic<std::int64_t> cell{5};

  EXPECT_EQ(5, folly::atomic_fetch_max_cond(cell, 7));
  EXPECT_EQ(7, cell.load());
  EXPECT_EQ(7, folly::atomic_fetch_min_cond(cell, 3));
  EXPECT_EQ(3, cell.load());
}

struct AtomicFetchBitOpCondTest : testing::Test {};

TEST_F(AtomicFetchBitOpCondTest, setReset) {
  std::atomic<unsigned> cell{0b0100};

  EXPECT_TRUE(folly::atomic_fetch_set_cond(cell, 2, relaxed));
  EXPECT_EQ(0b0100, cell.load(relaxed));
  EXPECT_FALSE(folly::atomic_fetch_set_cond(cell, 1, relaxed));
  EXPECT_EQ(0b0110, cell.load(relaxed));

  EXPECT_FALSE(folly::atomic_fetch_reset_cond(cell, 3, relaxed));
  EXPECT_EQ(0b0110, cell.load(relaxed));
  EXPECT_TRUE(folly::atomic_fetch_reset_cond(cell, 2, relaxed));
  EXPECT_EQ(0b0010, cell.load(relaxed));
}

TEST_F(AtomicFetchBitOpCondTest, relaxedAtomic) {
  relaxed_atomic<unsigned> cell{0b0100};

  EXPECT_TRUE(folly::atomic_fetch_set_cond(cell, 2));
  EXPECT_FALSE(folly::atomic_fetch_set_cond(cell, 1));
  EXPECT_EQ(0b0110, cell.load());
  EXPECT_TRUE(folly::atomic_fetch_reset_cond(cell, 1));
  EXPECT_EQ(0b0100, cell.load());
}

//  the integral specializations of relaxed_atomic reach relaxed_atomic_base by
//  private inheritance and re-export its members
TEST_F(AtomicFetchMinMaxTest, relaxedAtomicIntegral) {
  relaxed_atomic<std::int64_t> cell{5};

  EXPECT_EQ(5, folly::atomic_fetch_min(cell, 3));
  EXPECT_EQ(3, cell.load());
  EXPECT_EQ(3, folly::atomic_fetch_min(cell, 7));
  EXPECT_EQ(3, cell.load());
  EXPECT_EQ(3, folly::atomic_fetch_max(cell, 7));
  EXPECT_EQ(7, cell.load());
}

//  the primary template of relaxed_atomic inherits relaxed_atomic_base publicly
TEST_F(AtomicFetchMinMaxTest, relaxedAtomicNonIntegral) {
  relaxed_atomic<double> cell{5};

  EXPECT_EQ(5, folly::atomic_fetch_min(cell, 3));
  EXPECT_EQ(3, cell.load());
  EXPECT_EQ(3, folly::atomic_fetch_max(cell, 7));
  EXPECT_EQ(7, cell.load());
}

//  the value types below are the non-integral ones which appear at the call
//  sites these operations are meant to replace: floating point, chrono
//  durations and time-points, scoped enums, and strong typedefs. none reaches
//  the native member path, so all exercise the c/x-loop fallback.

namespace {

enum class AtomicFetchMinMaxEnum : int { lo = 1, mid = 2, hi = 3 };

//  a strong typedef which is trivially copyable and ordered but deliberately
//  NOT default-constructible, since the member-detection traits spell the
//  value type as atomic_value_type_t<Atomic>{}
struct AtomicFetchMinMaxStrong {
  int value;

  explicit constexpr AtomicFetchMinMaxStrong(int v) noexcept : value{v} {}

  friend constexpr bool operator<(
      AtomicFetchMinMaxStrong a, AtomicFetchMinMaxStrong b) noexcept {
    return a.value < b.value;
  }
  friend constexpr bool operator==(
      AtomicFetchMinMaxStrong a, AtomicFetchMinMaxStrong b) noexcept {
    return a.value == b.value;
  }
};
static_assert(!std::is_default_constructible_v<AtomicFetchMinMaxStrong>);

static_assert( //
    !detail::has_atomic_fetch_max_member_v<
        std::atomic<AtomicFetchMinMaxStrong>>);

//  unlike std::atomic<AtomicFetchMinMaxStrong>, this one does have the members,
//  so value-initialization of the value type is the only thing left which can
//  defeat detection
struct AtomicFetchMinMaxMemberStrong {
  using value_type = AtomicFetchMinMaxStrong;

  value_type load(std::memory_order = seq_cst) const { return value; }

  bool compare_exchange_weak(
      value_type& expected, value_type desired, std::memory_order = seq_cst) {
    if (!(value == expected)) {
      expected = value;
      return false;
    }
    value = desired;
    return true;
  }

  value_type fetch_min(value_type arg, std::memory_order) {
    ++fetchMinCalls;
    return std::exchange(value, arg < value ? arg : value);
  }

  value_type fetch_max(value_type arg, std::memory_order) {
    ++fetchMaxCalls;
    return std::exchange(value, value < arg ? arg : value);
  }

  value_type value{5};
  int fetchMinCalls{0};
  int fetchMaxCalls{0};
};

//  value-initialization of a non-default-constructible value type must be a
//  substitution failure, not a hard error, so that such a type simply falls
//  back to the c/x loop
static_assert( //
    !detail::has_atomic_fetch_min_member_v<AtomicFetchMinMaxMemberStrong>);
static_assert( //
    !detail::has_atomic_fetch_max_member_v<AtomicFetchMinMaxMemberStrong>);

} // namespace

TEST_F(AtomicFetchMinMaxCondTest, floatingPoint) {
  std::atomic<double> cell{5.};

  EXPECT_EQ(5., folly::atomic_fetch_max_cond(cell, 3., relaxed));
  EXPECT_EQ(5., cell.load(relaxed));
  EXPECT_EQ(5., folly::atomic_fetch_max_cond(cell, 7.5, relaxed));
  EXPECT_EQ(7.5, cell.load(relaxed));
  EXPECT_EQ(7.5, folly::atomic_fetch_min_cond(cell, 2.5, relaxed));
  EXPECT_EQ(2.5, cell.load(relaxed));
}

//  floating point is the one value type here which operator< does not totally
//  order. with nan every comparison is false, so the guard never fires: a nan
//  argument never stores, and a nan already in the atomic is never displaced.
//  that is std::min / std::max behavior rather than fmin / fmax, which quiet
//  nan instead. it is total and deterministic, never undefined
//
//  but it is a property of the c/x-loop fallback, not of the interface, since
//  a native fetch_min / fetch_max member is free to quiet nan instead, so pin
//  it only where the fallback is what actually runs
TEST_F(AtomicFetchMinMaxCondTest, floatingPointNotANumber) {
  if constexpr (!detail::has_atomic_fetch_max_member_v<std::atomic<double>>) {
    constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
    std::atomic<double> cell{5.};

    EXPECT_EQ(5., folly::atomic_fetch_max_cond(cell, nan, relaxed));
    EXPECT_EQ(5., cell.load(relaxed));
    EXPECT_EQ(5., folly::atomic_fetch_min_cond(cell, nan, relaxed));
    EXPECT_EQ(5., cell.load(relaxed));

    cell.store(nan, relaxed);

    EXPECT_TRUE(std::isnan(folly::atomic_fetch_max_cond(cell, 7.5, relaxed)));
    EXPECT_TRUE(std::isnan(cell.load(relaxed)));
    EXPECT_TRUE(std::isnan(folly::atomic_fetch_min_cond(cell, 2.5, relaxed)));
    EXPECT_TRUE(std::isnan(cell.load(relaxed)));
  }
}

TEST_F(AtomicFetchMinMaxCondTest, chronoDuration) {
  using namespace std::chrono_literals;
  std::atomic<std::chrono::microseconds> cell{5us};

  EXPECT_EQ(5us, folly::atomic_fetch_max_cond(cell, 3us, relaxed));
  EXPECT_EQ(5us, cell.load(relaxed));
  EXPECT_EQ(5us, folly::atomic_fetch_max_cond(cell, 7us, relaxed));
  EXPECT_EQ(7us, cell.load(relaxed));
  EXPECT_EQ(7us, folly::atomic_fetch_min_cond(cell, 2us, relaxed));
  EXPECT_EQ(2us, cell.load(relaxed));
}

TEST_F(AtomicFetchMinMaxCondTest, chronoTimePoint) {
  using namespace std::chrono_literals;
  using time_point = std::chrono::steady_clock::time_point;
  std::atomic<time_point> cell{time_point{5us}};

  EXPECT_EQ(
      time_point{5us}, folly::atomic_fetch_max_cond(cell, time_point{3us}));
  EXPECT_EQ(time_point{5us}, cell.load());
  EXPECT_EQ(
      time_point{5us}, folly::atomic_fetch_max_cond(cell, time_point{7us}));
  EXPECT_EQ(time_point{7us}, cell.load());
  EXPECT_EQ(
      time_point{7us}, folly::atomic_fetch_min_cond(cell, time_point{2us}));
  EXPECT_EQ(time_point{2us}, cell.load());
}

TEST_F(AtomicFetchMinMaxCondTest, scopedEnum) {
  using enum_type = AtomicFetchMinMaxEnum;
  std::atomic<enum_type> cell{enum_type::mid};

  EXPECT_EQ(enum_type::mid, folly::atomic_fetch_max_cond(cell, enum_type::lo));
  EXPECT_EQ(enum_type::mid, cell.load());
  EXPECT_EQ(enum_type::mid, folly::atomic_fetch_max_cond(cell, enum_type::hi));
  EXPECT_EQ(enum_type::hi, cell.load());
  EXPECT_EQ(enum_type::hi, folly::atomic_fetch_min_cond(cell, enum_type::lo));
  EXPECT_EQ(enum_type::lo, cell.load());
}

TEST_F(AtomicFetchMinMaxCondTest, strongTypedef) {
  using strong = AtomicFetchMinMaxStrong;
  std::atomic<strong> cell{strong{5}};

  EXPECT_EQ(strong{5}, folly::atomic_fetch_max_cond(cell, strong{3}));
  EXPECT_EQ(strong{5}, cell.load());
  EXPECT_EQ(strong{5}, folly::atomic_fetch_max_cond(cell, strong{7}));
  EXPECT_EQ(strong{7}, cell.load());
  EXPECT_EQ(strong{7}, folly::atomic_fetch_min_cond(cell, strong{2}));
  EXPECT_EQ(strong{2}, cell.load());
}

//  the members are present but undetectable, so the c/x loop must run instead
TEST_F(
    AtomicFetchMinMaxCondTest,
    memberElidedWhenValueTypeNotDefaultConstructible) {
  using strong = AtomicFetchMinMaxStrong;
  AtomicFetchMinMaxMemberStrong cell;

  EXPECT_EQ(strong{5}, folly::atomic_fetch_max_cond(cell, strong{7}, relaxed));
  EXPECT_EQ(strong{7}, cell.load());
  EXPECT_EQ(0, cell.fetchMaxCalls);

  EXPECT_EQ(strong{7}, folly::atomic_fetch_min_cond(cell, strong{2}, relaxed));
  EXPECT_EQ(strong{2}, cell.load());
  EXPECT_EQ(0, cell.fetchMinCalls);

  //  the counters are live, so the zero-checks above are not vacuous: calling
  //  the members directly does reach them and does increment
  EXPECT_EQ(strong{2}, cell.fetch_max(strong{9}, relaxed));
  EXPECT_EQ(strong{9}, cell.load());
  EXPECT_EQ(1, cell.fetchMaxCalls);

  EXPECT_EQ(strong{9}, cell.fetch_min(strong{1}, relaxed));
  EXPECT_EQ(strong{1}, cell.load());
  EXPECT_EQ(1, cell.fetchMinCalls);
}

//  the unconditional forms take the same value types
TEST_F(AtomicFetchMinMaxTest, nonIntegralValueTypes) {
  using namespace std::chrono_literals;
  using enum_type = AtomicFetchMinMaxEnum;

  std::atomic<std::chrono::microseconds> duration{5us};
  EXPECT_EQ(5us, folly::atomic_fetch_max(duration, 7us, relaxed));
  EXPECT_EQ(7us, duration.load(relaxed));
  EXPECT_EQ(7us, folly::atomic_fetch_min(duration, 2us, relaxed));
  EXPECT_EQ(2us, duration.load(relaxed));

  std::atomic<enum_type> value{enum_type::mid};
  EXPECT_EQ(enum_type::mid, folly::atomic_fetch_max(value, enum_type::hi));
  EXPECT_EQ(enum_type::hi, value.load());
  EXPECT_EQ(enum_type::hi, folly::atomic_fetch_min(value, enum_type::lo));
  EXPECT_EQ(enum_type::lo, value.load());

  std::atomic<AtomicFetchMinMaxStrong> strong{AtomicFetchMinMaxStrong{5}};
  EXPECT_EQ(
      AtomicFetchMinMaxStrong{5},
      folly::atomic_fetch_max(strong, AtomicFetchMinMaxStrong{7}));
  EXPECT_EQ(AtomicFetchMinMaxStrong{7}, strong.load());
  EXPECT_EQ(
      AtomicFetchMinMaxStrong{7},
      folly::atomic_fetch_min(strong, AtomicFetchMinMaxStrong{2}));
  EXPECT_EQ(AtomicFetchMinMaxStrong{2}, strong.load());
}

} // namespace folly
