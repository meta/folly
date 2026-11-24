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

#include <folly/synchronization/Lock.h>

#include <functional>
#include <tuple>

#include <folly/portability/GTest.h>

using namespace std::literals::chrono_literals;

namespace {

//  a fake mutex type and associated types for use in testing

namespace q = folly::access;

using Clock = std::chrono::steady_clock;

class UnownedError : public std::runtime_error {
 public:
  UnownedError() : std::runtime_error::runtime_error("unowned") {}
};

class DeadlockError : public std::runtime_error {
 public:
  DeadlockError() : std::runtime_error::runtime_error("deadlock") {}
};

class MismatchError : public std::runtime_error {
 public:
  MismatchError() : std::runtime_error::runtime_error("mismatch") {}
};

enum class Held { None, Unique, Shared, Upgrade };

template <
    typename UniqueLockState,
    typename SharedLockState,
    typename UpgradeLockState>
class Mutex {
 private:
  //  template magic helpers:
  //  - ensure that lock functions return void or state
  //  - ensure that try-lock functions return bool or state
  //  - ensure that unlock functions take nothing or state
  //  - ensure that unlock-and-lock functions return void or state and take
  //        nothing or state
  //  - ensure that try-unlock-and-lock functions return bool or state and take
  //        nothing or state

  template <typename State>
  using v = State;

  template <typename State>
  using b = std::conditional_t<std::is_void_v<State>, bool, State>;

  template <typename State>
  struct a_ {
    template <typename... A>
    static inline constexpr bool apply =
        sizeof...(A) == 1 && std::is_constructible_v<State const&, A&&...>;
  };
  template <>
  struct a_<void> {
    template <typename... A>
    static inline constexpr bool apply = sizeof...(A) == 0;
  };
  template <typename State, typename... A>
  static inline constexpr bool a = a_<State>::template apply<A...>;

  template <typename... S>
  struct m_ {
    using self = m_<S...>;
    void operator()(S const&...);
    template <typename... A>
    static constexpr inline bool apply = std::is_invocable_v<self&, A...>;
  };
  template <typename... S>
  struct m_<void, S...> {
    using self = m_<void, S...>;
    void operator()(S const&...);
    template <typename... A>
    static constexpr inline bool apply = std::is_invocable_v<self&, A...>;
  };
  template <typename... S>
  using md_ = m_<S..., Clock::duration>;
  template <typename... S>
  using mt_ = m_<S..., Clock::time_point>;

  template <typename M, typename... A>
  static inline constexpr bool ap_ = M::template apply<A const&...>;

 public:
  Held held = Held::None;
  Clock::time_point now = Clock::now();
  Clock::time_point locked_until = Clock::time_point::min();

  v<UniqueLockState> lock() { //
    return op(Held::Unique), v<UniqueLockState>(1);
  }
  b<UniqueLockState> try_lock() {
    return b<UniqueLockState>(try_op(Held::Unique));
  }
  b<UniqueLockState> try_lock_for(Clock::duration timeout) {
    return b<UniqueLockState>(try_op_for(Held::Unique, timeout));
  }
  b<UniqueLockState> try_lock_until(Clock::time_point deadline) {
    return b<UniqueLockState>(try_op_until(Held::Unique, deadline));
  }
  template <typename... A>
    requires a<UniqueLockState, A...>
  void unlock(A&&... state) {
    unop(Held::Unique, state...);
  }

  v<SharedLockState> lock_shared() {
    return op(Held::Shared), v<SharedLockState>(1);
  }
  b<SharedLockState> try_lock_shared() {
    return b<SharedLockState>(try_op(Held::Shared));
  }
  b<SharedLockState> try_lock_shared_for(Clock::duration timeout) {
    return b<SharedLockState>(try_op_for(Held::Shared, timeout));
  }
  b<SharedLockState> try_lock_shared_until(Clock::time_point deadline) {
    return b<SharedLockState>(try_op_until(Held::Shared, deadline));
  }
  template <typename... A>
    requires a<SharedLockState, A...>
  void unlock_shared(A&&... state) {
    unop(Held::Shared, state...);
  }

  v<UpgradeLockState> lock_upgrade() {
    return op(Held::Upgrade), v<UpgradeLockState>(1);
  }
  b<UpgradeLockState> try_lock_upgrade() {
    return b<UpgradeLockState>(try_op(Held::Upgrade));
  }
  b<UpgradeLockState> try_lock_upgrade_for(Clock::duration timeout) {
    return b<UpgradeLockState>(try_op_for(Held::Upgrade, timeout));
  }
  b<UpgradeLockState> try_lock_upgrade_until(Clock::time_point deadline) {
    return b<UpgradeLockState>(try_op_until(Held::Upgrade, deadline));
  }
  template <typename... A>
    requires a<UpgradeLockState, A...>
  void unlock_upgrade(A&&... state) {
    unop(Held::Upgrade, state...);
  }

  template <typename... A>
    requires a<UniqueLockState, A...>
  v<SharedLockState> unlock_and_lock_shared(A&&... state) {
    return transition_0_(q::unlock, q::lock_shared, state...);
  }
  template <typename... A>
    requires a<UniqueLockState, A...>
  v<UpgradeLockState> unlock_and_lock_upgrade(A&&... state) {
    return transition_0_(q::unlock, q::lock_upgrade, state...);
  }
  template <typename... A>
    requires a<UpgradeLockState, A...>
  v<UniqueLockState> unlock_upgrade_and_lock(A&&... state) {
    return transition_0_(q::unlock_upgrade, q::lock, state...);
  }
  template <typename... A>
    requires a<UpgradeLockState, A...>
  v<SharedLockState> unlock_upgrade_and_lock_shared(A&&... state) {
    return transition_0_(q::unlock_upgrade, q::lock_shared, state...);
  }

  template <typename... A>
    requires ap_<m_<SharedLockState>, A...>
  b<UpgradeLockState> try_unlock_shared_and_lock_upgrade(A const&... a) {
    return transition_0_(q::unlock_shared, q::try_lock_upgrade, a...);
  }
  template <typename... A>
    requires ap_<md_<SharedLockState>, A...>
  b<UpgradeLockState> try_unlock_shared_and_lock_upgrade_for(A const&... a) {
    return transition_1_(q::unlock_shared, q::try_lock_upgrade_for, a...);
  }
  template <typename... A>
    requires ap_<mt_<SharedLockState>, A...>
  b<UpgradeLockState> try_unlock_shared_and_lock_upgrade_until(A const&... a) {
    return transition_1_(q::unlock_shared, q::try_lock_upgrade_until, a...);
  }

  template <typename... A>
    requires ap_<m_<SharedLockState>, A...>
  b<UniqueLockState> try_unlock_shared_and_lock(A const&... a) {
    return transition_0_(q::unlock_shared, q::try_lock, a...);
  }
  template <typename... A>
    requires ap_<md_<SharedLockState>, A...>
  b<UniqueLockState> try_unlock_shared_and_lock_for(A const&... a) {
    return transition_1_(q::unlock_shared, q::try_lock_for, a...);
  }
  template <typename... A>
    requires ap_<mt_<SharedLockState>, A...>
  b<UniqueLockState> try_unlock_shared_and_lock_until(A const&... a) {
    return transition_1_(q::unlock_shared, q::try_lock_until, a...);
  }

  template <typename... A>
    requires ap_<m_<UpgradeLockState>, A...>
  b<UniqueLockState> try_unlock_upgrade_and_lock(A const&... a) {
    return transition_0_(q::unlock_upgrade, q::try_lock, a...);
  }
  template <typename... A>
    requires ap_<md_<UpgradeLockState>, A...>
  b<UniqueLockState> try_unlock_upgrade_and_lock_for(A const&... a) {
    return transition_1_(q::unlock_upgrade, q::try_lock_for, a...);
  }
  template <typename... A>
    requires ap_<mt_<UpgradeLockState>, A...>
  b<UniqueLockState> try_unlock_upgrade_and_lock_until(A const&... a) {
    return transition_1_(q::unlock_upgrade, q::try_lock_until, a...);
  }

 private:
  //  impl ...

  template <bool V>
  struct s_ {
    s_() = default;
    template <typename S>
      requires std::is_constructible_v<bool, S>
    /* implicit */ s_(S const& s) {
      !!s == V ? void() : folly::throw_exception<MismatchError>();
    }
  };

  void op(Held h) { try_op(h) || (throw DeadlockError(), 0); }
  bool try_op(Held h) { return try_op_for(h, Clock::duration::zero()); }
  bool try_op_for(Held h, Clock::duration timeout) {
    return try_op_until(h, now + timeout);
  }
  bool try_op_until(Held h, Clock::time_point deadline) {
    held == Held::None || (throw DeadlockError(), 0);
    auto const locked =
        locked_until != Clock::time_point::max() && locked_until <= deadline;
    held = locked ? h : held;
    now = locked ? locked_until : deadline;
    locked_until = locked ? Clock::time_point::max() : locked_until;
    return locked;
  }
  void unop(Held h, s_<1> = {}) {
    held == Held::None && (throw UnownedError(), 0);
    held == h || (throw MismatchError(), 0);
    locked_until == Clock::time_point::min() && (throw UnownedError(), 0);
    held = Held::None;
    locked_until = Clock::time_point::min();
  }
  template <size_t... I, typename... A>
  decltype(auto) init_(std::index_sequence<I...>, A&&... a) {
    auto t = std::forward_as_tuple(static_cast<A&&>(a)...);
    return std::forward_as_tuple(std::get<I>(t)...);
  }
  template <typename Unlock, typename Relock, typename... A>
  auto transition_0_(Unlock u, Relock r, A const&... a) {
    return u(*this, a...), r(*this);
  }
  template <typename Unlock, typename Relock, typename... A>
  auto transition_1_(Unlock u, Relock r, A const&... a) {
    static_assert(sizeof...(A) > 0);
    auto last = std::get<sizeof...(A) - 1>(std::forward_as_tuple(a...));
    auto seq = std::make_index_sequence<sizeof...(A)>{};
    return std::apply(u, init_(seq, *this, a...)), r(*this, last);
  }
};

template <Held>
class LockState {
 public:
  constexpr LockState() = default;
  explicit constexpr LockState(bool held) noexcept : held_{held} {}
  constexpr LockState(LockState const&) = default;
  constexpr LockState& operator=(LockState const&) = default;
  explicit constexpr operator bool() const { return held_; }

 private:
  bool held_{false};
};

using UniqueLockState = LockState<Held::Unique>;
using SharedLockState = LockState<Held::Shared>;
using UpgradeLockState = LockState<Held::Upgrade>;

} // namespace

namespace std {

template <typename X, typename S, typename U>
class unique_lock<Mutex<X, S, U>>
    : public folly::unique_lock_base<Mutex<X, S, U>> {
  using folly::unique_lock_base<Mutex<X, S, U>>::unique_lock_base;
};

template <typename X, typename S, typename U>
class shared_lock<Mutex<X, S, U>>
    : public folly::shared_lock_base<Mutex<X, S, U>> {
  using folly::shared_lock_base<Mutex<X, S, U>>::shared_lock_base;
};

template <typename X, typename S, typename U>
class lock_guard<Mutex<X, S, U>>
    : public folly::unique_lock_guard_base<Mutex<X, S, U>> {
  using folly::unique_lock_guard_base<Mutex<X, S, U>>::unique_lock_guard_base;
};

} // namespace std

//  general helpers for use across test types

template <typename Param>
using param_mutex_t = Mutex<
    typename Param::unique_lock_state,
    typename Param::shared_lock_state,
    typename Param::upgrade_lock_state>;
template <typename Param>
using param_lock_t = typename Param::template lock_type<param_mutex_t<Param>>;
template <typename Param>
using param_state_t = folly::detail::lock_state_type_of_t<param_lock_t<Param>>;
template <typename Param>
using param_from_lock_t =
    typename Param::template from_lock_type<param_mutex_t<Param>>;
template <typename Param>
using param_to_lock_t =
    typename Param::template to_lock_type<param_mutex_t<Param>>;

template <typename L>
[[maybe_unused]] static constexpr Held held_v = Held::None;
template <typename M>
static constexpr Held held_v<std::unique_lock<M>> = Held::Unique;
template <typename M>
static constexpr Held held_v<std::shared_lock<M>> = Held::Shared;
template <typename M>
static constexpr Held held_v<folly::upgrade_lock<M>> = Held::Upgrade;

template <typename M>
using x = std::unique_lock<M>;
template <typename M>
using s = std::shared_lock<M>;
template <typename M>
using u = folly::upgrade_lock<M>;

template <typename M>
using xg = folly::unique_lock_guard<M>;
template <typename M>
using sg = folly::shared_lock_guard<M>;

template <typename>
struct lock_guard_lock_t_;
template <typename M>
struct lock_guard_lock_t_<folly::unique_lock_guard<M>> {
  using type = std::unique_lock<M>;
};
template <typename M>
struct lock_guard_lock_t_<folly::shared_lock_guard<M>> {
  using type = std::shared_lock<M>;
};
template <typename LG>
using lock_guard_lock_t = typename lock_guard_lock_t_<LG>::type;
template <typename LG>
using lock_guard_state_t =
    folly::detail::lock_state_type_of_t<lock_guard_lock_t<LG>>;

//  combinatorial test suite for lock types
//
//  combinations:
//  - lock is: x (unique), s (shared), u (upgrade)?
//  - lock has state?
//
//  lower x, s, u denote locks sans state
//  upper X, S, U denote locks with state

template <int X, int S, int U, template <typename> class L>
struct LockTestParam {
  using unique_lock_state = std::conditional_t<X, UniqueLockState, void>;
  using shared_lock_state = std::conditional_t<S, SharedLockState, void>;
  using upgrade_lock_state = std::conditional_t<U, UpgradeLockState, void>;
  template <typename M>
  using lock_type = L<M>;
};

template <typename Param>
struct LockTest : testing::TestWithParam<Param> {
  template <typename... A>
  using deduction_unique = decltype(std::unique_lock{FOLLY_DECLVAL(A)...});
  template <typename... A>
  using deduction_shared = decltype(std::shared_lock{FOLLY_DECLVAL(A)...});
  template <typename... A>
  using deduction_upgrade = decltype(folly::upgrade_lock{FOLLY_DECLVAL(A)...});

  template <typename Expected, typename Actual>
  static constexpr void static_assert_same() {
    static_assert(std::is_same_v<Expected, Actual>);
  }

  static constexpr void check_deductions() {
    using mutex_type = param_mutex_t<Param>;
    using unique_lock_state = typename Param::unique_lock_state;
    using shared_lock_state = typename Param::shared_lock_state;
    using upgrade_lock_state = typename Param::upgrade_lock_state;

    static_assert_same<
        std::unique_lock<mutex_type>,
        deduction_unique<mutex_type&>>();
    static_assert_same<
        std::shared_lock<mutex_type>,
        deduction_shared<mutex_type&>>();
    static_assert_same<
        folly::upgrade_lock<mutex_type>,
        deduction_upgrade<mutex_type&>>();

    if constexpr (std::is_void_v<unique_lock_state>) {
      static_assert_same<
          std::unique_lock<mutex_type>,
          deduction_unique<mutex_type&, std::adopt_lock_t>>();
    } else {
      static_assert_same<
          std::unique_lock<mutex_type>,
          deduction_unique<
              mutex_type&,
              folly::adopt_lock_state_t,
              unique_lock_state>>();
    }

    if constexpr (std::is_void_v<shared_lock_state>) {
      static_assert_same<
          std::shared_lock<mutex_type>,
          deduction_shared<mutex_type&, std::adopt_lock_t>>();
    } else {
      static_assert_same<
          std::shared_lock<mutex_type>,
          deduction_shared<
              mutex_type&,
              folly::adopt_lock_state_t,
              shared_lock_state>>();
    }

    if constexpr (std::is_void_v<upgrade_lock_state>) {
      static_assert_same<
          folly::upgrade_lock<mutex_type>,
          deduction_upgrade<mutex_type&, std::adopt_lock_t>>();
    } else {
      static_assert_same<
          folly::upgrade_lock<mutex_type>,
          deduction_upgrade<
              mutex_type&,
              folly::adopt_lock_state_t,
              upgrade_lock_state>>();
    }
  }

  LockTest() { //
    check_deductions();
  }
};
TYPED_TEST_SUITE_P(LockTest);

TYPED_TEST_P(LockTest, ctor) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;
  using state_type = param_state_t<TypeParam>;

  mutex_type m;
  std::ignore = lock_type{m};
  std::ignore = lock_type{m, std::try_to_lock};
  std::ignore = lock_type{m, std::defer_lock};

  lock_type l{m};
  if constexpr (std::is_void_v<state_type>) {
    std::ignore = lock_type{*l.release(), std::adopt_lock};
  } else {
    auto s = l.state();
    std::ignore = lock_type{*l.release(), std::adopt_lock, s};
  }
  std::ignore = lock_type{m, 1s};
  std::ignore = lock_type{m, m.now + 1s};
}

TYPED_TEST_P(LockTest, construct_default) {
  using lock_type = param_lock_t<TypeParam>;

  lock_type l;
  EXPECT_EQ(nullptr, l.mutex());
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, construct_mutex) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m};
  EXPECT_EQ(held_v<lock_type>, m.held);
  EXPECT_EQ(&m, l.mutex());
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, construct_mutex_defer) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m, std::defer_lock};
  EXPECT_EQ(Held::None, m.held);
  EXPECT_EQ(&m, l.mutex());
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, construct_mutex_try_pass) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m, std::try_to_lock};
  EXPECT_EQ(held_v<lock_type>, m.held);
  EXPECT_EQ(&m, l.mutex());
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, construct_mutex_try_fail) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  m.locked_until = m.now + 1s;
  lock_type l{m, std::try_to_lock};
  EXPECT_EQ(Held::None, m.held);
  EXPECT_EQ(&m, l.mutex());
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, construct_mutex_adopt) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;
  using state_type = param_state_t<TypeParam>;

  mutex_type m;
  m.held = held_v<lock_type>;
  m.locked_until = m.now + 1s;
  lock_type l = std::invoke([&] {
    if constexpr (std::is_void_v<state_type>) {
      return lock_type{m, std::adopt_lock};
    } else {
      return lock_type{m, std::adopt_lock, state_type{true}};
    }
  });
  EXPECT_EQ(held_v<lock_type>, m.held);
  EXPECT_EQ(&m, l.mutex());
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, construct_mutex_duration_pass) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  m.locked_until = m.now + 1s;
  lock_type l{m, 1000ms};
  EXPECT_EQ(held_v<lock_type>, m.held);
  EXPECT_EQ(&m, l.mutex());
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, construct_mutex_duration_fail) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  m.locked_until = m.now + 1s;
  lock_type l{m, 999ms};
  EXPECT_EQ(Held::None, m.held);
  EXPECT_EQ(&m, l.mutex());
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, construct_mutex_time_point_pass) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  m.locked_until = m.now + 1s;
  lock_type l{m, m.now + 1000ms};
  EXPECT_EQ(held_v<lock_type>, m.held);
  EXPECT_EQ(&m, l.mutex());
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, construct_mutex_time_point_fail) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  m.locked_until = m.now + 1s;
  lock_type l{m, m.now + 999ms};
  EXPECT_EQ(Held::None, m.held);
  EXPECT_EQ(&m, l.mutex());
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, move_construct) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l0{m};
  EXPECT_EQ(&m, l0.mutex());
  EXPECT_TRUE(l0.owns_lock());
  lock_type l1{std::move(l0)};
  EXPECT_EQ(nullptr, l0.mutex());
  EXPECT_FALSE(l0.owns_lock());
  EXPECT_EQ(&m, l1.mutex());
  EXPECT_TRUE(l1.owns_lock());
  l1.unlock();
  EXPECT_EQ(&m, l1.mutex());
  EXPECT_FALSE(l1.owns_lock());
  lock_type l2{std::move(l1)};
  EXPECT_EQ(nullptr, l1.mutex());
  EXPECT_FALSE(l1.owns_lock());
  EXPECT_EQ(&m, l2.mutex());
  EXPECT_FALSE(l2.owns_lock());
}

TYPED_TEST_P(LockTest, destruct) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  std::optional<lock_type> lock;
  lock.emplace(m);
  EXPECT_EQ(held_v<lock_type>, m.held);
  lock.reset();
  EXPECT_EQ(Held::None, m.held);
  lock.emplace(m, std::defer_lock);
  EXPECT_EQ(Held::None, m.held);
  lock.reset();
  EXPECT_EQ(Held::None, m.held);
}

TYPED_TEST_P(LockTest, move_assign) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m0;
  mutex_type m1;
  lock_type l0{m0};
  EXPECT_EQ(&m0, l0.mutex());
  EXPECT_TRUE(l0.owns_lock());
  lock_type l1{m1};
  EXPECT_EQ(&m1, l1.mutex());
  EXPECT_TRUE(l1.owns_lock());
  EXPECT_EQ(held_v<lock_type>, m0.held);
  EXPECT_EQ(held_v<lock_type>, m1.held);
  l1 = std::move(l0);
  EXPECT_EQ(held_v<lock_type>, m0.held);
  EXPECT_EQ(Held::None, m1.held);
  EXPECT_EQ(nullptr, l0.mutex());
  EXPECT_FALSE(l0.owns_lock());
  EXPECT_EQ(&m0, l1.mutex());
  EXPECT_TRUE(l1.owns_lock());
  l1.unlock();
  EXPECT_EQ(Held::None, m0.held);
  EXPECT_EQ(Held::None, m1.held);
}

TYPED_TEST_P(LockTest, lock_pass) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m, std::defer_lock};
  l.lock();
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, lock_fail) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  m.locked_until = Clock::time_point::max();
  lock_type l{m, std::defer_lock};
  EXPECT_THROW(l.lock(), DeadlockError);
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, lock_owns) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m};
  EXPECT_THROW(l.lock(), std::system_error);
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, lock_empty) {
  using lock_type = param_lock_t<TypeParam>;

  lock_type l{};
  EXPECT_THROW(l.lock(), std::system_error);
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_pass) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m, std::defer_lock};
  EXPECT_TRUE(l.try_lock());
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_fail) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  m.locked_until = m.now + 1s;
  lock_type l{m, std::defer_lock};
  EXPECT_FALSE(l.try_lock());
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_owns) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m};
  EXPECT_THROW(l.try_lock(), std::system_error);
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_empty) {
  using lock_type = param_lock_t<TypeParam>;

  lock_type l{};
  EXPECT_THROW(l.try_lock(), std::system_error);
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_for_pass) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  m.locked_until = m.now + 1s;
  lock_type l{m, std::defer_lock};
  EXPECT_TRUE(l.try_lock_for(2s));
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_for_fail) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  m.locked_until = m.now + 1s;
  lock_type l{m, std::defer_lock};
  EXPECT_FALSE(l.try_lock_for(500ms));
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_for_owns) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m};
  EXPECT_THROW(l.try_lock_for(0s), std::system_error);
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_for_empty) {
  using lock_type = param_lock_t<TypeParam>;

  lock_type l{};
  EXPECT_THROW(l.try_lock_for(0s), std::system_error);
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_until_pass) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  m.locked_until = m.now + 1s;
  lock_type l{m, std::defer_lock};
  EXPECT_TRUE(l.try_lock_until(m.now + 2s));
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_until_fail) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  m.locked_until = m.now + 1s;
  lock_type l{m, std::defer_lock};
  EXPECT_FALSE(l.try_lock_until(m.now + 500ms));
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_until_owns) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m};
  EXPECT_THROW(l.try_lock_until(m.now), std::system_error);
  EXPECT_TRUE(l.owns_lock());
}

TYPED_TEST_P(LockTest, try_lock_until_empty) {
  using lock_type = param_lock_t<TypeParam>;

  lock_type l{};
  EXPECT_THROW(l.try_lock_until(Clock::now()), std::system_error);
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, unlock_owns) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m};
  l.unlock();
  EXPECT_FALSE(l.owns_lock());
}

TYPED_TEST_P(LockTest, unlock_unlocked) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;
  using state_type = param_state_t<TypeParam>;

  mutex_type m;
  lock_type l = std::invoke([&] {
    if constexpr (std::is_void_v<state_type>) {
      return lock_type{m, std::adopt_lock};
    } else {
      return lock_type{m, std::adopt_lock, state_type{true}};
    }
  });
  EXPECT_THROW(l.unlock(), UnownedError);
  l.release();
}

TYPED_TEST_P(LockTest, unlock_unowned) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m, std::defer_lock};
  EXPECT_THROW(l.unlock(), std::system_error);
}

TYPED_TEST_P(LockTest, unlock_empty) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{};
  EXPECT_THROW(l.unlock(), std::system_error);
}

TYPED_TEST_P(LockTest, release_empty) {
  using lock_type = param_lock_t<TypeParam>;

  lock_type l{};
  auto r = l.release();
  EXPECT_EQ(nullptr, r);
  EXPECT_EQ(nullptr, l.mutex());
  EXPECT_FALSE(l.owns_lock());
  EXPECT_EQ(nullptr, l.release());
}

TYPED_TEST_P(LockTest, release_unowned) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m, std::defer_lock};
  auto r = l.release();
  EXPECT_EQ(Held::None, m.held);
  EXPECT_EQ(&m, r);
  EXPECT_EQ(nullptr, l.mutex());
  EXPECT_FALSE(l.owns_lock());
  EXPECT_EQ(nullptr, l.release());
}

TYPED_TEST_P(LockTest, release_owns) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m;
  lock_type l{m};
  auto r = l.release();
  EXPECT_EQ(held_v<lock_type>, m.held);
  EXPECT_EQ(&m, r);
  EXPECT_EQ(nullptr, l.mutex());
  EXPECT_FALSE(l.owns_lock());
  EXPECT_EQ(nullptr, l.release());
}

TYPED_TEST_P(LockTest, swap_) { // gtest forces this mangling
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_type = param_lock_t<TypeParam>;

  mutex_type m0;
  mutex_type m1;
  lock_type l0{m0};
  lock_type l1{m1, std::defer_lock};
  swap(l0, l1);
  EXPECT_EQ(&m1, l0.mutex());
  EXPECT_FALSE(l0.owns_lock());
  EXPECT_EQ(&m0, l1.mutex());
  EXPECT_TRUE(l1.owns_lock());
}

REGISTER_TYPED_TEST_SUITE_P(
    LockTest,
    ctor,
    construct_default,
    construct_mutex,
    construct_mutex_defer,
    construct_mutex_try_pass,
    construct_mutex_try_fail,
    construct_mutex_adopt,
    construct_mutex_duration_pass,
    construct_mutex_duration_fail,
    construct_mutex_time_point_pass,
    construct_mutex_time_point_fail,
    move_construct,
    destruct,
    move_assign,
    lock_pass,
    lock_fail,
    lock_owns,
    lock_empty,
    try_lock_pass,
    try_lock_fail,
    try_lock_owns,
    try_lock_empty,
    try_lock_for_pass,
    try_lock_for_fail,
    try_lock_for_owns,
    try_lock_for_empty,
    try_lock_until_pass,
    try_lock_until_fail,
    try_lock_until_owns,
    try_lock_until_empty,
    unlock_owns,
    unlock_unlocked,
    unlock_unowned,
    unlock_empty,
    release_empty,
    release_unowned,
    release_owns,
    swap_);

INSTANTIATE_TYPED_TEST_SUITE_P(
    x1, LockTest, decltype(LockTestParam<0, 0, 0, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    X2, LockTest, decltype(LockTestParam<1, 0, 0, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    s1, LockTest, decltype(LockTestParam<0, 0, 0, s>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    S2, LockTest, decltype(LockTestParam<0, 1, 0, s>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    u1, LockTest, decltype(LockTestParam<0, 0, 0, u>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    U2, LockTest, decltype(LockTestParam<0, 0, 1, u>{}));

//  combinatorial test suite for lock guard types
//
//  combinations:
//  - lock is: x (unique), s (shared)
//  - lock has state?
//
//  lower x, s denote locks sans state
//  upper X, S denote locks with state

template <int X, int S, int U, template <typename> class L>
struct LockGuardTestParam {
  using unique_lock_state = std::conditional_t<X, UniqueLockState, void>;
  using shared_lock_state = std::conditional_t<S, SharedLockState, void>;
  using upgrade_lock_state = std::conditional_t<U, UpgradeLockState, void>;
  template <typename M>
  using lock_type = L<M>;
};

template <typename Param>
struct LockGuardTest : testing::TestWithParam<Param> {};
TYPED_TEST_SUITE_P(LockGuardTest);

TYPED_TEST_P(LockGuardTest, construct_mutex) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_guard_type = param_lock_t<TypeParam>;

  mutex_type m;
  {
    lock_guard_type l{m};
    EXPECT_EQ(held_v<lock_guard_lock_t<lock_guard_type>>, m.held);
  }
  EXPECT_EQ(Held::None, m.held);
}

TYPED_TEST_P(LockGuardTest, construct_mutex_adopt) {
  using mutex_type = param_mutex_t<TypeParam>;
  using lock_guard_type = param_lock_t<TypeParam>;
  using state_type = lock_guard_state_t<lock_guard_type>;

  mutex_type m;
  m.held = held_v<lock_guard_lock_t<lock_guard_type>>;
  m.locked_until = m.now + 1s;
  {
    lock_guard_type l = std::invoke([&] {
      if constexpr (std::is_void_v<state_type>) {
        return lock_guard_type{m, std::adopt_lock};
      } else {
        return lock_guard_type{m, std::adopt_lock, state_type{true}};
      }
    });
    EXPECT_EQ(held_v<lock_guard_lock_t<lock_guard_type>>, m.held);
  }
  EXPECT_EQ(Held::None, m.held);
}

REGISTER_TYPED_TEST_SUITE_P(
    LockGuardTest, //
    construct_mutex,
    construct_mutex_adopt);

INSTANTIATE_TYPED_TEST_SUITE_P(
    x1, LockGuardTest, decltype(LockGuardTestParam<0, 0, 0, xg>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    X2, LockGuardTest, decltype(LockGuardTestParam<1, 0, 0, xg>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    s1, LockGuardTest, decltype(LockGuardTestParam<0, 0, 0, sg>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    S2, LockGuardTest, decltype(LockGuardTestParam<0, 1, 0, sg>{}));

//  combinatorial test suite for lock transitions
//
//  combinations:
//  - from lock is: x (unique), s (shared), u (upgrade)?
//  - from lock has state?
//  - to lock is: x (unique), s (shared), u (upgrade)?
//  - to lock has state?
//
//  but limited to valid transitions:
//  - x -> s
//  - x -> u
//  - u -> s
//  - u -> x
//
//  lower x, s, u denote locks sans state
//  upper X, S, U denote locks with state

template <
    int X,
    int S,
    int U,
    template <typename> class FromL,
    template <typename> class ToL>
struct XLockTestParam {
  using unique_lock_state = std::conditional_t<X, UniqueLockState, void>;
  using shared_lock_state = std::conditional_t<S, SharedLockState, void>;
  using upgrade_lock_state = std::conditional_t<U, UpgradeLockState, void>;
  template <typename M>
  using from_lock_type = FromL<M>;
  template <typename M>
  using to_lock_type = ToL<M>;
};

template <typename Param>
struct TransitionLockTest : testing::TestWithParam<Param> {};
TYPED_TEST_SUITE_P(TransitionLockTest);

TYPED_TEST_P(TransitionLockTest, transition) {
  using mutex_type = param_mutex_t<TypeParam>;
  using from_lock_type = param_from_lock_t<TypeParam>;
  using to_lock_type = param_to_lock_t<TypeParam>;

  mutex_type m;
  from_lock_type l0{m};
  EXPECT_TRUE(l0.owns_lock());
  EXPECT_EQ(held_v<from_lock_type>, m.held);
  to_lock_type l1 =
      folly::transition_lock<TypeParam::template to_lock_type>(l0);
  EXPECT_TRUE(l1.owns_lock());
  EXPECT_FALSE(l0.owns_lock());
  EXPECT_EQ(held_v<to_lock_type>, m.held);
}

REGISTER_TYPED_TEST_SUITE_P(TransitionLockTest, transition);

INSTANTIATE_TYPED_TEST_SUITE_P(
    xs1, TransitionLockTest, decltype(XLockTestParam<0, 0, 0, x, s>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    Xs2, TransitionLockTest, decltype(XLockTestParam<1, 0, 0, x, s>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    xS3, TransitionLockTest, decltype(XLockTestParam<0, 1, 0, x, s>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    XS4, TransitionLockTest, decltype(XLockTestParam<1, 1, 0, x, s>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    xu1, TransitionLockTest, decltype(XLockTestParam<0, 0, 0, x, u>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    Xu2, TransitionLockTest, decltype(XLockTestParam<1, 0, 0, x, u>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    xU3, TransitionLockTest, decltype(XLockTestParam<0, 0, 1, x, u>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    XU4, TransitionLockTest, decltype(XLockTestParam<1, 0, 1, x, u>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    us1, TransitionLockTest, decltype(XLockTestParam<0, 0, 0, u, s>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    Us2, TransitionLockTest, decltype(XLockTestParam<0, 0, 1, u, s>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    uS3, TransitionLockTest, decltype(XLockTestParam<0, 1, 0, u, s>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    US4, TransitionLockTest, decltype(XLockTestParam<0, 1, 1, u, s>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    ux1, TransitionLockTest, decltype(XLockTestParam<0, 0, 0, u, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    Ux2, TransitionLockTest, decltype(XLockTestParam<0, 0, 1, u, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    uX3, TransitionLockTest, decltype(XLockTestParam<1, 0, 0, u, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    UX4, TransitionLockTest, decltype(XLockTestParam<1, 0, 1, u, x>{}));

//  combinatorial test suite for lock try-transitions
//
//  combinations:
//  - from lock is: x (unique), s (shared), u (upgrade)?
//  - from lock has state?
//  - to lock is: x (unique), s (shared), u (upgrade)?
//  - to lock has state?
//
//  but limited to valid try-transitions:
//  - s -> u
//  - s -> x
//  - u -> x
//
//  lower x, s, u denote locks sans state
//  upper X, S, U denote locks with state

template <typename Param>
struct TryTransitionLockTest : testing::TestWithParam<Param> {};
TYPED_TEST_SUITE_P(TryTransitionLockTest);

TYPED_TEST_P(TryTransitionLockTest, try_transition) {
  using mutex_type = param_mutex_t<TypeParam>;
  using from_lock_type = param_from_lock_t<TypeParam>;
  using to_lock_type = param_to_lock_t<TypeParam>;

  mutex_type m;
  from_lock_type l0{m};
  EXPECT_TRUE(l0.owns_lock());
  EXPECT_EQ(held_v<from_lock_type>, m.held);
  to_lock_type l1 =
      folly::try_transition_lock<TypeParam::template to_lock_type>(l0);
  EXPECT_TRUE(l1.owns_lock());
  EXPECT_FALSE(l0.owns_lock());
  EXPECT_EQ(held_v<to_lock_type>, m.held);
}

TYPED_TEST_P(TryTransitionLockTest, try_transition_for) {
  using mutex_type = param_mutex_t<TypeParam>;
  using from_lock_type = param_from_lock_t<TypeParam>;
  using to_lock_type = param_to_lock_t<TypeParam>;

  mutex_type m;
  from_lock_type l0{m};
  EXPECT_TRUE(l0.owns_lock());
  EXPECT_EQ(held_v<from_lock_type>, m.held);
  to_lock_type l1 =
      folly::try_transition_lock_for<TypeParam::template to_lock_type>(l0, 1s);
  EXPECT_TRUE(l1.owns_lock());
  EXPECT_FALSE(l0.owns_lock());
  EXPECT_EQ(held_v<to_lock_type>, m.held);
}

TYPED_TEST_P(TryTransitionLockTest, try_transition_until) {
  using mutex_type = param_mutex_t<TypeParam>;
  using from_lock_type = param_from_lock_t<TypeParam>;
  using to_lock_type = param_to_lock_t<TypeParam>;

  mutex_type m;
  from_lock_type l0{m};
  EXPECT_TRUE(l0.owns_lock());
  EXPECT_EQ(held_v<from_lock_type>, m.held);
  to_lock_type l1 =
      folly::try_transition_lock_until<TypeParam::template to_lock_type>(
          l0, m.now + 1s);
  EXPECT_TRUE(l1.owns_lock());
  EXPECT_FALSE(l0.owns_lock());
  EXPECT_EQ(held_v<to_lock_type>, m.held);
}

REGISTER_TYPED_TEST_SUITE_P(
    TryTransitionLockTest,
    try_transition,
    try_transition_for,
    try_transition_until);

INSTANTIATE_TYPED_TEST_SUITE_P(
    su1, TryTransitionLockTest, decltype(XLockTestParam<0, 0, 0, s, u>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    Su2, TryTransitionLockTest, decltype(XLockTestParam<0, 1, 0, s, u>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    sU3, TryTransitionLockTest, decltype(XLockTestParam<0, 0, 1, s, u>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    SU4, TryTransitionLockTest, decltype(XLockTestParam<0, 1, 1, s, u>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    sx1, TryTransitionLockTest, decltype(XLockTestParam<0, 0, 0, s, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    Sx2, TryTransitionLockTest, decltype(XLockTestParam<0, 1, 0, s, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    sX3, TryTransitionLockTest, decltype(XLockTestParam<1, 0, 0, s, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    SX4, TryTransitionLockTest, decltype(XLockTestParam<1, 1, 0, s, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    ux1, TryTransitionLockTest, decltype(XLockTestParam<0, 0, 0, u, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    Ux2, TryTransitionLockTest, decltype(XLockTestParam<0, 0, 1, u, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    uX3, TryTransitionLockTest, decltype(XLockTestParam<1, 0, 0, u, x>{}));
INSTANTIATE_TYPED_TEST_SUITE_P(
    UX4, TryTransitionLockTest, decltype(XLockTestParam<1, 0, 1, u, x>{}));
