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

#include <folly/coro/GtestHelpers.h>
#include <folly/coro/Timeout.h>
#include <folly/coro/ValueOrFatal.h>
#include <folly/coro/safe/AsyncClosure.h>
#include <folly/fibers/Semaphore.h>
#include <folly/init/Init.h>

#if FOLLY_HAS_COROUTINES

namespace folly::coro {

using namespace folly::bind::literals;
using namespace std::literals::chrono_literals;

CO_TEST(AsyncClosure, invalid_co_cleanup) {
  auto checkCleanup = []<typename T>(tag_t<T>) {
    return async_closure(
        bind::capture_in_place<T>(),
        [](auto) -> closure_task<void> { co_return; });
  };

  struct ValidCleanup : NonCopyableNonMovable {
    value_or_fatal<Task<>, on_stopped_void> co_cleanup(
        async_closure_private_t) {
      co_return;
    }
  };
  co_await checkCleanup(tag<ValidCleanup>);

  struct InvalidCleanupNonVoid : NonCopyableNonMovable {
    [[maybe_unused]] value_or_fatal<Task<int>, on_stopped<0>> co_cleanup(
        async_closure_private_t) {
      co_return 1;
    }
  };
#if 0 // Manual test -- this uses `static_assert` for better UX.
  co_await checkCleanup(tag<InvalidCleanupNonVoid>);
#endif

  struct InvalidCleanupLacksNoexcept : NonCopyableNonMovable {
    [[maybe_unused]] Task<void> co_cleanup(async_closure_private_t) {
      co_return;
    }
  };
#if 0 // Manual test -- this uses `static_assert` for better UX.
  co_await checkCleanup(tag<InvalidCleanupLacksNoexcept>);
#endif

  struct InvalidCleanupIsMovable {
    [[maybe_unused]] value_or_fatal<Task<>, on_stopped_void> co_cleanup(
        async_closure_private_t) {
      co_return;
    }
  };
#if 0 // Manual test -- this failure escapes `is_detected_v`.
  co_await checkCleanup(tag<InvalidCleanupIsMovable>);
#endif
}

static_assert(std::is_same_v<
              decltype(folly::coro::detail::cumsum_except_last<0, 2, 1, 3>),
              const vtag_t<0, 2, 3>>);

closure_task<int> intTask(int x) {
  co_return x;
}
struct StatelessIntCallable {
  closure_task<int> operator()(int x) { co_return x; }
};
struct StatelessGenericCallable {
  closure_task<int> operator()(auto x) { co_return x; }
};

// We can't directly test `async_closure*` for unsafe inputs, since that
// would trigger `static_assert`s in `release_outer_coro()`.  Instead, test
// `is_safe()` which verifies the same conditions.
template <bool ForceOuter>
void checkSafety() {
  constexpr int x = 42;

  auto safeWrap = [](auto fn, auto&& bargs) {
    return folly::coro::detail::
        async_closure_impl<ForceOuter, /*EmitNowTask*/ false>(
            std::move(bargs), std::move(fn));
  };

  // Check safe usage, with various levels of arg safety.
  // Covers: fn ptrs, plain & generic lambdas, callable & generic callables.
  safe_alias_constant<safe_alias::maybe_value> kValue;
  auto checkIsSafe = [&](auto arg_safety, auto fn, auto bargs) {
    auto s = safeWrap(std::move(fn), std::move(bargs));
    static_assert(s.is_safe());
    static_assert(
        folly::coro::detail::safe_task_traits<
            decltype(std::move(s).release_outer_coro())>::arg_safety ==
        arg_safety.value);
  };

  checkIsSafe(kValue, intTask, bind::args{5});
  checkIsSafe(kValue, StatelessIntCallable{}, bind::args{5});
  checkIsSafe(kValue, StatelessGenericCallable{}, bind::args{5});
  checkIsSafe(kValue, []() -> closure_task<int> { co_return 5; }, bind::args{});
  checkIsSafe(kValue, []() -> closure_task<void> { co_return; }, bind::args{});
  checkIsSafe(
      kValue, [](int x) -> closure_task<int> { co_return x; }, bind::args{5});
  checkIsSafe(
      kValue, [](auto) -> closure_task<void> { co_return; }, bind::args{5});
  checkIsSafe(
      safe_alias_constant<safe_alias::co_cleanup_safe_ref>{},
      [](auto) -> closure_task<void> { co_return; },
      bind::args{manual_safe_ref<safe_alias::co_cleanup_safe_ref>(x)});
  checkIsSafe(
      safe_alias_constant<safe_alias::after_cleanup_ref>{},
      [](auto) -> closure_task<void> { co_return; },
      bind::args{manual_safe_ref<safe_alias::after_cleanup_ref>(x)});

  auto checkIsUnsafe = [&](auto fn, auto bargs) {
    auto s = safeWrap(std::move(fn), std::move(bargs));
    static_assert(!s.is_safe());
  };
  // Only `safe_task` is allowed as the inner coro.
  checkIsUnsafe([]() -> Task<int> { co_return 5; }, bind::args{});
  checkIsUnsafe([]() -> Task<void> { co_return; }, bind::args{});
  checkIsUnsafe([](int x) -> Task<int> { co_return x; }, bind::args{5});
  checkIsUnsafe([](auto) -> Task<void> { co_return; }, bind::args{5});
  // Don't allow passing in `unsafe*` args externally.
  checkIsUnsafe(
      [](auto) -> closure_task<void> { co_return; },
      bind::args{manual_safe_ref<safe_alias::unsafe_closure_internal>(x)});
}

TEST(AsyncClosure, safetyNoOuter) {
  checkSafety</*force outer*/ false>();
}
TEST(AsyncClosure, safety) {
  checkSafety</*force outer*/ true>();
}

inline constexpr async_closure_config ForceOuter{.force_outer_coro = true};
inline constexpr async_closure_config NoForceOuter{.force_outer_coro = false};

// Checks that `async_closure` returns the `safe_task` we expect.
template <typename ExpectedT, async_closure_config Cfg = NoForceOuter>
constexpr auto asyncClosureCheckType(auto fn, auto bargs) {
  auto t = async_closure<Cfg>(
      // Actually, safe because `bargs` is by-value
      bind::ext::unsafe_move_args::from(std::move(bargs)),
      std::move(fn));
  static_assert(std::is_same_v<decltype(t), ExpectedT>);
  return std::move(t);
}

template <async_closure_config Cfg>
Task<void> checkNoArgs() {
  auto res = co_await asyncClosureCheckType<value_task<int>, Cfg>(
      []() -> closure_task<int> { co_return 7; }, bind::args{});
  EXPECT_EQ(7, res);
}

CO_TEST(AsyncClosure, noArgsNoOuter) {
  co_await checkNoArgs<NoForceOuter>();
}
CO_TEST(AsyncClosure, noArgs) {
  co_await checkNoArgs<ForceOuter>();
}

namespace {
static bool ran_returnsVoid;
}

template <async_closure_config Cfg>
Task<void> checkReturnsVoid() {
  ran_returnsVoid = false;
  co_await asyncClosureCheckType<value_task<void>, Cfg>(
      []() -> closure_task<void> {
        ran_returnsVoid = true;
        co_return;
      },
      bind::args{});
  EXPECT_TRUE(ran_returnsVoid);
}

CO_TEST(AsyncClosure, returnsVoidNoOuter) {
  co_await checkReturnsVoid<NoForceOuter>();
}
CO_TEST(AsyncClosure, returnsVoid) {
  co_await checkReturnsVoid<ForceOuter>();
}

template <async_closure_config Cfg>
Task<void> checkPlainArgs() {
  int thirtySix = 36; // test passing l-values
  auto res = co_await asyncClosureCheckType<value_task<int>, Cfg>(
      [](int x, auto yPtr, const auto z) -> closure_task<int> {
        ++x;
        int r = x + *yPtr + z;
        yPtr.reset();
        // Plain args have plain types
        static_assert(std::is_same_v<std::unique_ptr<int>, decltype(yPtr)>);
        co_return r;
      },
      bind::args{thirtySix, std::make_unique<int>(1200), 100});
  EXPECT_EQ(1337, res);
}

CO_TEST(AsyncClosure, plainArgsNoOuter) {
  co_await checkPlainArgs<NoForceOuter>();
}
CO_TEST(AsyncClosure, plainArgsOuter) {
  co_await checkPlainArgs<ForceOuter>();
}

closure_task<std::string> funcTemplate(auto hi) {
  *hi += "de-and-seek";
  co_return std::move(*hi);
}

CO_TEST(AsyncClosure, callFuncTemplate) {
  auto res = co_await asyncClosureCheckType<value_task<std::string>>(
      // As of 2024, C++ lacks an "overload set" type, and thus can't
      // directly deduce `funcTemplate` (see P3360R0 pr P3312R0).
      FOLLY_INVOKE_QUAL(funcTemplate),
      bind::args{bind::capture_in_place<std::string>("hi")});
  EXPECT_EQ("hide-and-seek", res);
}

// With `bind::capture()`, immovable objects get auto-promoted to
// `capture_heap<>` iff the closure's outer coro is elided.
struct ImmovableString : private NonCopyableNonMovable {
  explicit ImmovableString(std::string s) : s_(std::move(s)) {}
  std::string s_;
};

// When needed, closure callbacks can have explicit & readable type signatures.
// Unfortunately, the signature depends on whether the closure has an outer
// coro wrapping the inner one.
closure_task<std::string> funcNoOuter(capture_heap<ImmovableString> hi) {
  hi->s_ += "de-and-seek";
  co_return std::move(hi->s_);
}
closure_task<std::string> funcWithOuter(capture<ImmovableString&> hi) {
  hi->s_ += "de-and-seek";
  co_return std::move(hi->s_);
}

CO_TEST(AsyncClosure, callFunctionNoOuter) {
  auto res = co_await asyncClosureCheckType<value_task<std::string>>(
      funcNoOuter, bind::args{bind::capture_in_place<ImmovableString>("hi")});
  EXPECT_EQ("hide-and-seek", res);
}

CO_TEST(AsyncClosure, callFunctionWithOuter) {
  auto res =
      co_await asyncClosureCheckType<value_task<std::string>, ForceOuter>(
          funcWithOuter,
          bind::args{bind::capture_in_place<ImmovableString>("hi")});
  EXPECT_EQ("hide-and-seek", res);
}

struct TakesBackref {
  capture<std::string&> prefix_;
  std::string suffix_;
};

CO_TEST(AsyncClosure, captureBackref) {
  auto concat_prefix_suffix =
      [](auto, auto /*hello*/, auto world) -> closure_task<std::string> {
    co_return *world->prefix_ + world->suffix_;
  };

  auto r1 = co_await asyncClosureCheckType<value_task<std::string>, ForceOuter>(
      concat_prefix_suffix,
      bind::args{
          "s1"_id = bind::capture(std::string{"goodbye"}),
          "s2"_id = bind::capture(std::string{"hello"}),
          bind::capture_in_place<TakesBackref>("s2"_id, " world!")});
  EXPECT_EQ("hello world!", r1);

  auto r2 = co_await asyncClosureCheckType<value_task<std::string>, ForceOuter>(
      concat_prefix_suffix,
      bind::args{
          "s1"_id = bind::capture(std::string{"goodbye"}),
          "s2"_id = bind::capture(std::string{"hello"}),
          bind::capture_in_place<TakesBackref>("s1"_id, " world!")});
  EXPECT_EQ("goodbye world!", r2);

#if 0 // manual test for "backrefs must point only to the left" assert
  (void)asyncClosureCheckType<value_task<std::string>, ForceOuter>(
      concat_prefix_suffix,
      bind::args{
          "s1"_id = bind::capture(std::string{"goodbye"}),
          bind::capture_in_place<TakesBackref>("s2"_id, " world!"),
          "s2"_id = bind::capture(std::string{"hello"})});
#endif

#if 0 // manual test for "ambiguous backref" scenario
  // Future: Make this error message clearer than the current:
  //   error: no matching function for call to 'async_closure_backref_get'
  (void)asyncClosureCheckType<value_task<std::string>, ForceOuter>(
      concat_prefix_suffix,
      bind::args{
          "s"_id = bind::capture(std::string{"goodbye"}),
          "s"_id = bind::capture(std::string{"hello"}),
          bind::capture_in_place<TakesBackref>("s"_id, " world!")});
#endif

#if 0 // manual test for "backref not found" scenario
  // Future: Make this error message clearer than the current:
  //   error: no matching function for call to 'async_closure_backref_get'
  (void)asyncClosureCheckType<value_task<std::string>, ForceOuter>(
      concat_prefix_suffix,
      bind::args{
          "x1"_id = bind::capture(std::string{"goodbye"}),
          "x2"_id = bind::capture(std::string{"hello"}),
          bind::capture_in_place<TakesBackref>("s"_id, " world!")});
#endif
}

CO_TEST(AsyncClosure, simpleCancellation) {
  EXPECT_THROW(
      co_await timeout(
          async_closure(
              bind::args{},
              []() -> closure_task<void> {
                folly::fibers::Semaphore stuck{0}; // a cancellable baton
                co_await stuck.co_wait();
              }),
          200ms),
      folly::FutureTimeout);
}

struct InPlaceOnly : folly::NonCopyableNonMovable {
  explicit InPlaceOnly(bool* made, int n) : n_(n) {
    if (made) {
      *made = true;
    }
  }
  int n_;
};

void assertArgConst(auto& arg) {
  static_assert(std::is_const_v<std::remove_reference_t<decltype(*arg)>>);
  static_assert(
      std::is_const_v<std::remove_pointer_t<decltype(arg.operator->())>>);
}

template <async_closure_config Cfg>
Task<void> checkInPlaceArgs() {
  bool made = false;
  auto res = co_await asyncClosureCheckType<value_task<int>, Cfg>(
      [](int a, auto b, auto c, auto d) -> closure_task<int> {
        static_assert(
            std::is_same_v<
                decltype(b),
                std::conditional_t<
                    Cfg.force_outer_coro,
                    capture<int&>,
                    capture<int>>>);
        *b += 100;
        static_assert(
            std::is_same_v<
                decltype(c),
                std::conditional_t<
                    Cfg.force_outer_coro,
                    capture<const InPlaceOnly&>,
                    capture_heap<const InPlaceOnly>>>);
        assertArgConst(c); // `const` underlying type
        assertArgConst(d); // marked `constant`
        co_return a + *b + c->n_ + *d;
      },
      bind::args{
          30, // a
          // Test both const and non-const `AsyncOuterClosurePtr`s.
          // Check that "x"_id tagging for capture backrefs is transparent.
          "b"_id = bind::capture(1000),
          "c"_id = bind::capture_in_place<const InPlaceOnly>(&made, 7),
          bind::capture(bind::constant(200))}); // d
  EXPECT_EQ(1337, res);
  EXPECT_TRUE(made);
}

CO_TEST(AsyncClosure, inPlaceArgsNoOuter) {
  co_await checkInPlaceArgs<NoForceOuter>();
}
CO_TEST(AsyncClosure, inPlaceArgs) {
  co_await checkInPlaceArgs<ForceOuter>();
}

// Tests that, with an outer coro, the user can specify `const auto`
// args on the inner task, and they work as expected.
//
// IIUC this can't work generically for the "no outer coro" scenario, since
// args need to be copied or moved into the inner coro, and non-copyable,
// `const` classes are not movable.  In `checkInPlaceArgs()`, you can see
// the workaround of passing a `const` (or equivalenly `constant()`) arg.
CO_TEST(AsyncClosureTest, constAutoArgWithOuterCoro) {
  bool made = false;
  auto res = co_await asyncClosureCheckType<value_task<int>, ForceOuter>(
      [](const auto a) -> closure_task<int> {
        static_assert(
            std::is_same_v<decltype(a), const capture<const InPlaceOnly&>>);
        assertArgConst(a);
        co_return a->n_;
      },
      bind::args{bind::capture(
          bind::in_place<
// Manual test: When set to 0, this should fail to compile because the `const
// auto` above requires (via `FOLLY_MOVABLE_AND_DEEP_CONST_LREF_COPYABLE`) the
// inner type to be `const`.
#if 1
              const
#endif
              InPlaceOnly>(&made, 7))});
  EXPECT_EQ(7, res);
  EXPECT_TRUE(made);
}

// A simple test pair showing the "move-in" vs "by-ref" behavior of the "no
// outer coro" optimization. The `nestedRefs*` tests elaborate on this.
CO_TEST(AsyncClosure, noOuterCoroGetsCaptureValue) {
  co_await async_closure(bind::capture(1337), [](auto n) -> closure_task<void> {
    static_assert(std::is_same_v<decltype(n), capture<int>>);
    co_return;
  });
}
CO_TEST(AsyncClosure, outerCoroGetsCaptureRef) {
  co_await async_closure<ForceOuter>(
      bind::capture(1337), [](auto n) -> closure_task<void> {
        static_assert(std::is_same_v<decltype(n), capture<int&>>);
        co_return;
      });
}

CO_TEST(AsyncClosure, nestedRefsWithOuterCoro) {
  auto res = co_await asyncClosureCheckType<value_task<int>, ForceOuter>(
      [](auto x, const auto y, const auto z) -> closure_task<int> {
        static_assert(std::is_same_v<decltype(x), capture<int&>>);
        static_assert(
            std::is_same_v<
                decltype(y),
                const capture<const std::unique_ptr<int>&>>);
        assertArgConst(y);
        static_assert(
            std::is_same_v<
                decltype(z),
                const capture_indirect<const std::unique_ptr<int>&>>);
        *x += 100;
        co_await asyncClosureCheckType<co_cleanup_safe_task<void>>(
            [](auto x2, auto y2, auto z2) -> closure_task<void> {
              static_assert(std::is_same_v<decltype(x2), capture<int&>>);
              static_assert(
                  std::is_same_v<
                      decltype(y2),
                      capture<const std::unique_ptr<int>&>>);
              assertArgConst(y2);
              static_assert(
                  std::is_same_v<
                      decltype(z2),
                      capture_indirect<const std::unique_ptr<int>&>>);
              *x2 += 100; // ref remains non-const -- C++ arg semantics
              co_return;
            },
            bind::args{x, y, z});
        // Can also pass `capture<Ref>`s into a bare safe_task.
        co_await [](auto x3, auto y3, auto z3) -> co_cleanup_safe_task<void> {
          static_assert(std::is_same_v<decltype(x3), capture<int&>>);
          static_assert(
              std::is_same_v<
                  decltype(y3),
                  capture<const std::unique_ptr<int>&>>);
          assertArgConst(y3);
          static_assert(
              std::is_same_v<
                  decltype(z3),
                  capture_indirect<const std::unique_ptr<int>&>>);
          *x3 += 100; // ref remains non-const -- C++ arg semantics
          co_return;
        }(x, y, z);
        co_return *x + **y + *z;
      },
      bind::args{
          bind::capture(
              bind::in_place<int>(1000),
              bind::constant(std::make_unique<int>(23))),
          bind::capture_indirect(bind::constant(std::make_unique<int>(14)))});
  EXPECT_EQ(1337, res);
}

// Like `ImmovableString`, this helps us detect when the outer coro was elided
struct ImmovableInt : private NonCopyableNonMovable {
  explicit ImmovableInt(int n) : n_(std::move(n)) {}
  int n_;
};

// We want this to be as similar as possible to `nestedRefsWithOuterCoro` --
// after all, "no outer coro" is supposed to be a "mostly transparent"
// optimization. Therefore, the main differences are:
//   - Split `x` into `w` and `x` to cover both heap and non-heap behaviors.
//   - `capture`s move into the inner coro, and therefore cannot:
//     * Write `const auto y` or `const auto z`, which would need a copy ctor
//     * Use `constant()` around `std::make_unique()` (prevents move).
//   - Correspondingly, we have to drop the `const`ness asserts.
//   - To pass `capture<Val>` into a bare `safe_task`, we now have to
//     explicitly declare the its argument types, to use the implicit
//     conversion from `capture<Val>` to `capture<Val&>`.
CO_TEST(AsyncClosure, nestedRefsWithoutOuterCoro) {
  auto res = co_await asyncClosureCheckType<value_task<int>, NoForceOuter>(
      [](auto w, auto x, auto y, auto z) -> closure_task<int> {
        // Only the immovable type gets promoted to `capture_heap`.
        static_assert(std::is_same_v<decltype(w), capture<int>>);
        static_assert(std::is_same_v<decltype(x), capture_heap<ImmovableInt>>);
        static_assert(
            std::is_same_v<
                decltype(z),
                capture_indirect<std::unique_ptr<const int>>>);
        x->n_ += 100;
        co_await asyncClosureCheckType<co_cleanup_safe_task<void>>(
            [](auto w2, auto y2, auto z2) -> closure_task<void> {
              static_assert(std::is_same_v<decltype(w2), capture<int&>>);
              static_assert(
                  std::is_same_v<decltype(y2), capture<std::unique_ptr<int>&>>);
              static_assert(
                  std::is_same_v<
                      decltype(z2),
                      capture_indirect<std::unique_ptr<const int>&>>);
              *w2 += 100; // ref remains non-const -- C++ arg semantics
              co_return;
            },
            bind::args{w, y, z});
        // Can pass implicitly converted `capture<Ref>`s into a safe_task
        co_await
            [](capture<ImmovableInt&> x3,
               capture<std::unique_ptr<int>&> y3,
               capture_indirect<std::unique_ptr<const int>&>)
                -> co_cleanup_safe_task<void> {
              x3->n_ += 50;
              *(*y3) += 50;
              co_return;
            }(x, y, z);
        co_return *w + x->n_ + **y + *z;
      },
      bind::args{
          bind::capture(
              bind::in_place<int>(700),
              bind::in_place<ImmovableInt>(300),
              std::make_unique<int>(23)),
          // Can't use `constant()` here because we can't move a `const
          // unique_ptr`.
          bind::capture_indirect(std::make_unique<const int>(14))});
  EXPECT_EQ(1337, res);
}

struct ErrorObliviousHasCleanup : NonCopyableNonMovable {
  explicit ErrorObliviousHasCleanup(int* p) : cleanBits_(p) {}
  int* cleanBits_;
  value_or_fatal<Task<>, on_stopped_void> co_cleanup(async_closure_private_t) {
    *cleanBits_ += 3;
    co_return;
  }
};

CO_TEST(AsyncClosure, errorObliviousCleanup) {
  int cleanBits = 0;
  co_await async_closure(
      bind::capture_in_place<ErrorObliviousHasCleanup>(&cleanBits),
      [](auto) -> closure_task<void> { co_return; });
  EXPECT_EQ(3, cleanBits);
}

struct HasCleanup : NonCopyableNonMovable {
  explicit HasCleanup(auto* p) : optCleanupErrPtr_(p) {}
  std::optional<exception_wrapper>* optCleanupErrPtr_;
  // If the closure (not other cleanups!) exited with an exception, each
  // `co_cleanup` gets to see it.
  value_or_fatal<Task<>, on_stopped_void> co_cleanup(
      async_closure_private_t, const exception_wrapper* ew) {
    *optCleanupErrPtr_ = *ew;
    co_return;
  }
};

CO_TEST(AsyncClosure, cleanupAfterSuccess) {
  std::optional<exception_wrapper> optCleanErr;
  co_await async_closure(
      bind::capture_in_place<HasCleanup>(&optCleanErr),
      [](auto) -> closure_task<void> { co_return; });
  EXPECT_FALSE(optCleanErr->has_exception_ptr());
}

CO_TEST(AsyncClosure, cleanupAfterError) {
  struct MagicError : std::exception {
    explicit MagicError(int m) : magic_(m) {}
    int magic_;
  };

  std::optional<exception_wrapper> optCleanErr;
  auto res = co_await co_awaitTry(async_closure(
      bind::capture_in_place<HasCleanup>(&optCleanErr),
      [](auto) -> closure_task<void> {
        co_yield folly::coro::co_error{MagicError{111}};
      }));
  EXPECT_EQ(111, optCleanErr->get_exception<MagicError>()->magic_);
  EXPECT_EQ(111, res.tryGetExceptionObject<MagicError>()->magic_);
}

struct CustomDerefCleanupProxy : NonCopyableNonMovable {
  explicit CustomDerefCleanupProxy(int y) : y_(y) {}
  auto operator->() { return static_cast<CustomDerefCleanupProxy*>(this); }
  int y_;
};

struct CustomDerefCleanup : HasCleanup {
  explicit CustomDerefCleanup(auto* p) : HasCleanup(p) {}
  using KindT = folly::coro::ext::capture_proxy_kind;
  template <KindT Kind, folly::coro::ext::const_or_not<CustomDerefCleanup> T>
  friend auto capture_proxy(folly::coro::ext::capture_proxy_tag<Kind>, T&) {
    if constexpr (Kind == KindT::lval_ref) {
      return CustomDerefCleanupProxy{101 + 1000 * std::is_const_v<T>};
    } else if constexpr (Kind == KindT::lval_ptr) {
      return CustomDerefCleanupProxy{202 + 1000 * std::is_const_v<T>};
    } else if constexpr (Kind == KindT::rval_ref) {
      return CustomDerefCleanupProxy{303 + 1000 * std::is_const_v<T>};
    } else if constexpr (Kind == KindT::rval_ptr) {
      return CustomDerefCleanupProxy{404 + 1000 * std::is_const_v<T>};
    } else {
      static_assert(false);
    }
  }
};

template <typename CleanupT>
Task<void> check_pass_cleanup_arg_to_subclosure(auto validate_ref) {
  std::optional<exception_wrapper> optCleanErr;
  co_await async_closure(
      bind::args{bind::capture_in_place<CleanupT>(&optCleanErr), validate_ref},
      [](auto c, auto validate_ref2) -> closure_task<void> {
        validate_ref2(c);
        static_assert(
            std::is_same_v<decltype(c), co_cleanup_capture<CleanupT&>>);
        co_await async_closure(
            bind::args{c, validate_ref2},
            [](auto c2, auto validate_ref3) -> closure_task<void> {
              validate_ref3(c2);
              static_assert(
                  std::is_same_v<decltype(c2), co_cleanup_capture<CleanupT&>>);
              co_return;
            });
      });
  EXPECT_FALSE(optCleanErr->has_exception_ptr());
}

CO_TEST(AsyncClosure, passCleanupArgToSubclosure) {
  co_await check_pass_cleanup_arg_to_subclosure<HasCleanup>([](auto&) {});
}
// Check that the "custom dereferencing" code doesn't break the automatic
// passing of `capture` refs to child closures.
CO_TEST(AsyncClosure, passCustomDerefCleanupArgToSubclosure) {
  co_await check_pass_cleanup_arg_to_subclosure<CustomDerefCleanup>(
      [](auto& c) {
        EXPECT_EQ(101, (*c).y_);
        EXPECT_EQ(202, c->y_);
        EXPECT_EQ(404, std::move(c)->y_);

        EXPECT_EQ(1101, (*std::as_const(c)).y_);
        EXPECT_EQ(1202, std::as_const(c)->y_);
        EXPECT_EQ(1404, std::move(std::as_const(c))->y_);
      });
}

TEST(AsyncClosure, nonSafeTaskIsNotAwaited) {
  bool awaited = false;
  auto lambda = [&]() -> Task<void> {
    awaited = true;
    co_return;
  };
  // We can't `release_outer_coro()` on either since they have a
  // `static_assert` -- but `checkIsUnsafe` above checks the logic.
  folly::coro::detail::async_closure_impl<
      /*ForceOuter*/ false,
      /*EmitNowTask*/ false>(bind::args{}, lambda);
  folly::coro::detail::async_closure_impl<
      /*ForceOuter*/ true,
      /*EmitNowTask*/ false>(bind::args{}, lambda);
  EXPECT_FALSE(awaited);
}

struct HasMemberTask {
  int z = 1300; // Goal: ASAN failures if the class is destroyed
  member_task<int> task(auto x, auto y) { co_return x + *y + z; }
  // An explicit safety annotation is required to use `member_task`
  template <safe_alias>
  using folly_private_safe_alias_t =
      safe_alias_constant<safe_alias::maybe_value>;
};

CO_TEST(AsyncClosure, memberTask) {
  // First, examples of a "bound" member closure that actually owns the object:
  EXPECT_EQ(
      1337,
      co_await async_closure<ForceOuter>(
          bind::args{bind::capture(HasMemberTask{}), 30, bind::capture(7)},
          FOLLY_INVOKE_MEMBER(task)));
  EXPECT_EQ(
      1337, // Syntax sugar: implicit `bind::capture` for member's object
            // parameter
      co_await async_closure<ForceOuter>(
          bind::args{HasMemberTask{}, 30, bind::capture(7)},
          FOLLY_INVOKE_MEMBER(task)));
  EXPECT_EQ(
      1337, // Same, but showing that `bind::in_place` still works
      co_await async_closure<ForceOuter>(
          bind::args{bind::in_place<HasMemberTask>(), 30, bind::capture(7)},
          FOLLY_INVOKE_MEMBER(task)));
  HasMemberTask hmt;
  EXPECT_EQ(
      1337, // Wouldn't compile without either `std::move` or `folly::copy`.
      co_await async_closure<ForceOuter>(
          bind::args{std::move(hmt), 30, bind::capture(7)},
          FOLLY_INVOKE_MEMBER(task)));

  // Second, call a member coro on an existing `capture<HasMemberTask>`.
  EXPECT_EQ(
      1337,
      co_await async_closure<ForceOuter>(
          bind::capture(HasMemberTask{}), [](auto mt) -> closure_task<int> {
            co_return co_await async_closure(
                bind::args{mt, 30, bind::capture(7)},
                FOLLY_INVOKE_MEMBER(task));
          }));
}

// Check that `async_now_closure` returns `now_task<int>` & return the task.
now_task<int> intAsyncNowClosure(auto&& bargs, auto&& fn) {
  return async_now_closure(
      bind::ext::unsafe_move_args::from(std::move(bargs)), std::move(fn));
}

template <typename T>
now_task<void> check_now_closure_no_outer_coro_unsafe_task() {
  int b1 = 300, c = 30, d = 7;
  // The coro take raw references & use lambda captures
  int res1 = co_await intAsyncNowClosure(
      bind::args{bind::capture(1000), b1}, [&c, d](auto a, int& b2) -> T {
        // No ref upgrade, since `T` is unsafe (`Task` or `now_task`)
        static_assert(std::is_same_v<after_cleanup_capture<int>, decltype(a)>);
        co_return *a + b2 + c + d;
      });
  EXPECT_EQ(1337, res1);

  // Same "no ref upgrade" test, but with a concrete arg type.
  int res2 = co_await intAsyncNowClosure(
      bind::capture(42),
      [](after_cleanup_capture<int> a) -> T { co_return *a; });
  EXPECT_EQ(42, res2);

  // Same "no ref upgrade" test, but with a parent ref forcing shared-cleanup
  std::optional<exception_wrapper> optCleanErr;
  int res3 = co_await async_now_closure(
      bind::capture_in_place<HasCleanup>(&optCleanErr),
      [](auto cleanup) -> closure_task<int> {
        co_return co_await intAsyncNowClosure(
            bind::args{cleanup, bind::capture(5)},
            [](auto, auto a) -> closure_task<int> {
              // No ref upgrade despite safe task, due to `cleanup` arg.
              static_assert(
                  std::is_same_v<after_cleanup_capture<int>, decltype(a)>);
              co_return *a;
            });
      });
  EXPECT_EQ(5, res3);
}

// The plumbing for an outer-coro closure is different, so test it too.
template <typename T>
now_task<void> check_now_closure_with_outer_coro() {
  int cleanBits = 128;
  int res = co_await intAsyncNowClosure(
      bind::capture_in_place<ErrorObliviousHasCleanup>(&cleanBits),
      [](auto c) -> T { co_return *c->cleanBits_; });
  EXPECT_EQ(128, res);
}

CO_TEST(AsyncClosure, nowClosure) {
  co_await check_now_closure_no_outer_coro_unsafe_task<Task<int>>();
  co_await check_now_closure_no_outer_coro_unsafe_task<now_task<int>>();

  co_await check_now_closure_with_outer_coro<Task<int>>();
  co_await check_now_closure_with_outer_coro<now_task<int>>();

  // Going from `closure_task` / `member_task` to `now_task` is rare, but it
  // does work.  Of course, passing raw refs is not possible in this case.

  co_await check_now_closure_with_outer_coro<closure_task<int>>();

  auto makeNowClosure =
      []<typename TaskT, typename CaptureRef>(tag_t<TaskT, CaptureRef>) {
        return intAsyncNowClosure(bind::capture(7), [](auto n) -> TaskT {
          static_assert(std::is_same_v<CaptureRef, decltype(n)>);
          co_return *n;
        });
      };
  // Safe `closure_task` -- safe to do a ref upgrade, absent `co_cleanup` args
  // from the parent.
  EXPECT_EQ(7, co_await makeNowClosure(tag<closure_task<int>, capture<int>>));
  EXPECT_EQ( // Unsafe `Task` -- no ref upgrade
      7,
      co_await makeNowClosure(tag<Task<int>, after_cleanup_capture<int>>));

  HasMemberTask hmt;
  auto memberRes = co_await intAsyncNowClosure(
      bind::args{&hmt, 7, bind::capture(30)}, FOLLY_INVOKE_MEMBER(task));
  EXPECT_EQ(1337, memberRes);
}

CO_TEST(AsyncClosure, captureByReference) {
  // This demo uses an atomic because e.g. with async scopes, the inner tasks
  // might be concurrent -- and you can't move atomics, so you either need to
  // use `AfterCleanup.h` (preferred, safer!) or capture-by-reference.
  std::atomic_int n = 0;
  co_await async_now_closure(
      bind::capture_mut_ref{n}, [](auto n) -> closure_task<void> {
        static_assert(std::is_same_v<capture<std::atomic_int&>, decltype(n)>);
        n->fetch_add(42);
        co_return;
      });
  EXPECT_EQ(42, n.load());
  // Same, but with shared-cleanup downgrade due to an unsafe inner task type
  co_await async_now_closure(
      bind::capture_mut_ref{n}, [](auto n) -> Task<void> {
        static_assert(
            std::is_same_v<
                after_cleanup_capture<std::atomic_int&>,
                decltype(n)>);
        n->fetch_add(-5);
        co_return;
      });
  EXPECT_EQ(37, n.load());
}

template <typename TaskT, typename CaptureIntRef>
now_task<> checkNowClosureCoCleanup() {
  std::optional<exception_wrapper> optCleanErr;
  int res = co_await async_now_closure(
      bind::args{
          bind::capture_in_place<HasCleanup>(&optCleanErr),
          bind::capture(1300)},
      [](auto cleanup, auto n) -> TaskT {
        static_assert(
            std::is_same_v<co_cleanup_capture<HasCleanup&>, decltype(cleanup)>);
        static_assert(std::is_same_v<CaptureIntRef, decltype(n)>);
        co_return *n + 37;
      });
  EXPECT_EQ(1337, res);
  EXPECT_TRUE(optCleanErr.has_value());
}

CO_TEST(AsyncClosure, nowClosureCoCleanup) {
  co_await checkNowClosureCoCleanup<closure_task<int>, capture<int&>>();
  // Downgraded ref safety, since `Task` is unsafe
  co_await checkNowClosureCoCleanup<Task<int>, after_cleanup_capture<int&>>();
}

constexpr bool check_value_or_fatal_closures() {
  static_assert( // safe_task, without outer coro
      std::is_same_v<
          value_or_fatal<value_task<>, on_stopped_void>,
          decltype(async_closure(
              bind::args{},
              []() -> value_or_fatal<closure_task<>, on_stopped_void> {
                co_return;
              }))>);

  static_assert( // safe_task, with outer coro
      std::is_same_v<
          value_or_fatal<value_task<>, on_stopped_void>,
          decltype(async_closure<ForceOuter>(
              bind::args{},
              []() -> value_or_fatal<closure_task<>, on_stopped_void> {
                co_return;
              }))>);

  static_assert( // now_task, without outer coro
      std::is_same_v<
          value_or_fatal<now_task<>, on_stopped_void>,
          decltype(async_now_closure(
              bind::args{}, []() -> value_or_fatal<Task<>, on_stopped_void> {
                co_return;
              }))>);
  static_assert( // now_task, with outer coro
      std::is_same_v<
          value_or_fatal<now_task<>, on_stopped_void>,
          decltype(async_now_closure<ForceOuter>(
              bind::args{}, []() -> value_or_fatal<Task<>, on_stopped_void> {
                co_return;
              }))>);

  return true;
}

static_assert(check_value_or_fatal_closures());

TEST(AsyncClosure, throwingMoveCtor) {
  struct MyErr : std::exception {};

  struct ThrowOnMove {
    ThrowOnMove() {}
    ~ThrowOnMove() = default;
    [[noreturn]] ThrowOnMove(ThrowOnMove&&) { throw MyErr{}; }
    ThrowOnMove(const ThrowOnMove&) = delete;
    void operator=(ThrowOnMove&&) = delete;
    void operator=(const ThrowOnMove&) = delete;
  };

  // This used to also test `value_or_fatal<closure_task<ThrowingMove>...>`,
  // but this no longer compiles since value-only awaitables don't make any
  // sense with throwing move constructors.

  auto throwNoOuter = async_closure(
      bind::args{}, []() -> closure_task<ThrowOnMove> { co_return {}; });
  EXPECT_THROW(blockingWait(std::move(throwNoOuter)), MyErr);

  auto throwOuter = async_closure<ForceOuter>(
      bind::args{}, []() -> closure_task<ThrowOnMove> { co_return {}; });
  EXPECT_THROW(blockingWait(std::move(throwOuter)), MyErr);
}

// Records construction order, asserts that (1) cleanup & destruction happen in
// the opposite order, and (2) all cleanups complete before any dtors.
struct OrderTracker : NonCopyableNonMovable {
  int myN_;
  int& nRef_;
  int myCleanupN_;
  int& cleanupNRef_;

  explicit OrderTracker(int& n, int& cleanupN)
      : myN_(++n), nRef_(n), myCleanupN_(++cleanupN), cleanupNRef_(cleanupN) {}

  value_or_fatal<Task<>, on_stopped_void> co_cleanup(async_closure_private_t) {
    EXPECT_EQ(myCleanupN_, cleanupNRef_--);
    co_return;
  }
  ~OrderTracker() {
    // Our contract is that all cleanups complete before any capture is
    // destroyed.  This is required for `AfterCleanup.h` to be useful.
    EXPECT_EQ(1000, cleanupNRef_);
    EXPECT_EQ(myN_, nRef_--);
  }
};

CO_TEST(AsyncClosure, ctorCleanupDtorOrdering) {
  int n = 0, cleanupN = 1000;
  co_await async_closure(
      bind::args{
          bind::capture_in_place<OrderTracker>(n, cleanupN),
          bind::capture_in_place<OrderTracker>(n, cleanupN),
          bind::capture_in_place<OrderTracker>(n, cleanupN),
          bind::capture_in_place<OrderTracker>(n, cleanupN)},
      [](auto c1, auto c2, auto c3, auto c4) -> closure_task<void> {
        EXPECT_EQ(4, c1->nRef_);
        EXPECT_EQ(1, c1->myN_);
        EXPECT_EQ(2, c2->myN_);
        EXPECT_EQ(3, c3->myN_);
        EXPECT_EQ(4, c4->myN_);

        EXPECT_EQ(1004, c1->cleanupNRef_);
        EXPECT_EQ(1001, c1->myCleanupN_);
        EXPECT_EQ(1002, c2->myCleanupN_);
        EXPECT_EQ(1003, c3->myCleanupN_);
        EXPECT_EQ(1004, c4->myCleanupN_);

        co_return;
      });
}

} // namespace folly::coro

#endif

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  folly::Init init(&argc, &argv); // `timeout` uses a `Timekeeper` singleton
  return RUN_ALL_TESTS();
}
