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

#include <folly/coro/Task.h>
#include <folly/coro/ValueOrFatal.h>
#include <folly/coro/safe/detail/BindAsyncClosure.h>
#include <folly/portability/GTest.h>

#if FOLLY_HAS_COROUTINES

namespace folly::coro::detail {

struct HasCleanup : NonCopyableNonMovable {
  value_or_fatal<Task<>, on_stopped_void> co_cleanup(async_closure_private_t) {
    co_return;
  }
};

constexpr capture_private_t coro_safe_detail_bindings_test_private() {
  return capture_private_t{};
}
inline constexpr auto priv = coro_safe_detail_bindings_test_private();

// This is here so that test "runs" show up in CI history
TEST(BindingsTest, all_tests_run_at_build_time) {}

struct MoveMe : folly::MoveOnly {
  int x;
};

template <
    typename ExpectedT,
    safe_alias ExpectedSafety = safe_alias::maybe_value,
    bool ArgsForceSharedCleanup = false>
constexpr void check_one_no_shared_cleanup(auto arg_fn) {
  static_assert(
      std::is_same_v<
          decltype(async_closure_safeties_and_bindings<
                   async_closure_bindings_cfg{
                       .force_outer_coro = false,
                       .emit_now_task = false,
                       .is_invoke_member = false}>(bind::args{arg_fn()})),
          lite_tuple::tuple<
              async_closure_arg_safety<ExpectedSafety, ArgsForceSharedCleanup>,
              lite_tuple::tuple<ExpectedT>,
              std::nullptr_t>>);
}

template <
    typename ExpectedT,
    safe_alias ExpectedSafety = safe_alias::maybe_value>
constexpr void check_one_shared_cleanup(auto arg_fn) {
  constexpr auto ExpectedMinSafety =
      std::min({ExpectedSafety, safe_alias::shared_cleanup});
  static_assert(
      std::is_same_v<
          decltype(async_closure_safeties_and_bindings<
                   async_closure_bindings_cfg{
                       .force_outer_coro = false,
                       .emit_now_task = false,
                       .is_invoke_member = false}>(bind::args{
              arg_fn(),
              // Triggers "shared cleanup" downgrade logic.
              // NB: Without the second `&`, this would look like
              // `std::move(c)`, which is not allowed for cleanup captures.
              std::declval<co_cleanup_capture<HasCleanup&>&>()})),
          lite_tuple::tuple<
              async_closure_arg_safety<
                  ExpectedMinSafety,
                  /*args_force_shared_cleanup*/ true>,
              lite_tuple::tuple<ExpectedT, co_cleanup_capture<HasCleanup&>>,
              std::nullptr_t>>);
}

template <
    typename ExpectedT,
    safe_alias ExpectedSafety = safe_alias::maybe_value,
    bool ArgsForceSharedCleanup = false>
constexpr void check_one(auto arg_fn) {
  check_one_no_shared_cleanup<
      ExpectedT,
      ExpectedSafety,
      ArgsForceSharedCleanup>(arg_fn);
  check_one_shared_cleanup<ExpectedT, ExpectedSafety>(arg_fn);
}

constexpr bool check_empty() {
  static_assert(
      std::is_same_v<
          decltype(async_closure_safeties_and_bindings<
                   async_closure_bindings_cfg{
                       .force_outer_coro = false,
                       .emit_now_task = false,
                       .is_invoke_member = false}>(bind::args{})),
          lite_tuple::tuple<
              async_closure_arg_safety<
                  safe_alias::maybe_value,
                  /*args_force_shared_cleanup*/ false>,
              lite_tuple::tuple<>,
              std::nullptr_t>>);
  return true;
}

static_assert(check_empty());

// Somewhat redundant with `lang/bind/Bind.h` tests, but we should show that
// these work "as expected".  No `bind::in_place` coverage here since we don't
// allow those on the "regular" path.
constexpr bool check_regular_args() {
  int x = 7;
  check_one<async_closure_regular_arg<int, bind_wrapper_t<int&>>>(
      [&]() -> auto& { return x; });

  // Unsafe arg binding is relevant for `async_now_closure`
  check_one<
      async_closure_regular_arg<int*, bind_wrapper_t<int*&&>>,
      safe_alias::unsafe,
      /*args_force_shared_cleanup*/ true>([&]() -> auto* { return &x; });

  MoveMe moo{.x = 7};
  check_one<async_closure_regular_arg<MoveMe, bind_wrapper_t<MoveMe&&>>>(
      [&]() -> auto&& { return std::move(moo); });

  // The `MoveMe` test makes an rvalue ref from an lvalue, but this here is
  // actually passing a prvalue.  The reason we test both scenarios is that
  // it's completely fine to plumb prvalues through `async_closure`, since the
  // resulting task will either take it by-value, or it'll be a `now_task`.
  // Since `bind::args{}` objects are immovable, it's quite hard for a user to
  // accidentally grab a dangling ref to a prvalue this way.
  check_one<async_closure_regular_arg<int, bind_wrapper_t<int&&>>>([]() {
    return 5;
  });

  return true;
}

static_assert(check_regular_args());

// `check_capture_*_to_ref()` should closely parallel the corresponding
// `Captures.h` tests, but we still need to check the plumbing end-to-end.

constexpr bool check_capture_val_to_ref() {
  { // makeRef1: implicitly convert `capture<Val>` to `capture<Ref>`
    capture<int> av{priv, forward_bind_wrapper(5)};
    check_one<capture<int&>, safe_alias::co_cleanup_safe_ref>([&]() -> auto& {
      return av;
    });
    check_one<capture<int&&>, safe_alias::co_cleanup_safe_ref>([&]() -> auto&& {
      return std::move(av);
    });
  }

  { // makeRef1: auto-upgrade `after_cleanup_capture<Val>` to `capture<Ref>`
    after_cleanup_capture<int> av{priv, forward_bind_wrapper(5)};
    // IMPORTANT: Here and below, the closure's safety is `after_cleanup_ref`
    // **even though** the argument was upgraded.  That is, we must measure
    // the safety of the closure according to its pre-transformation
    // arguments.  For example, this buggy code should not compile due to the
    // `schedule()` closure measuring as `after_cleanup_ref`.
    //
    //   co_await async_closure(
    //       safeAsyncScope<CancelViaParent>(),
    //       [](auto scope) -> closure_task<void> {
    //         co_await async_closure(
    //             bind::args{scope, bind::capture(123)},
    //             [](auto outerScope, auto n) -> closure_task<void> {
    //               outerScope.with(co_await co_current_executor).schedule(
    //                   async_closure(
    //                       bind::args{n},
    //                       [](auto n2) -> closure_task<void> {
    //                         ++n2;
    //                         co_return;
    //                       }));
    //         	   });
    //         // BAD: The scheduled task may be running, but `n` is destroyed
    //       });
    check_one_no_shared_cleanup<capture<int&>, safe_alias::after_cleanup_ref>(
        [&]() -> auto& { return av; });
    check_one_no_shared_cleanup<capture<int&&>, safe_alias::after_cleanup_ref>(
        [&]() -> auto&& { return std::move(av); });
  }

  { // makeRef1: no automatic upgrade due to `shared_cleanup` arg
    after_cleanup_capture<int> av{priv, forward_bind_wrapper(5)};
    check_one_shared_cleanup<
        after_cleanup_capture<int&>,
        safe_alias::after_cleanup_ref>([&]() -> auto& { return av; });
    check_one_shared_cleanup<
        after_cleanup_capture<int&&>,
        safe_alias::after_cleanup_ref>([&]() -> auto&& {
      return std::move(av);
    });
  }

  { // makeRef2: `as_const(capture<Val>)` becomes `capture<const Ref>`
    capture<int> av{priv, forward_bind_wrapper(5)};
    check_one<capture<const int&>, safe_alias::co_cleanup_safe_ref>(
        [&]() -> auto& { return std::as_const(av); });
    check_one<capture<const int&&>, safe_alias::co_cleanup_safe_ref>(
        // NOLINTNEXTLINE(facebook-hte-ConstMove,performance-move-const-arg)
        [&]() -> auto&& { return std::move(std::as_const(av)); });
  }

  // Don't repeat the upgrade-related `makeRef1` tests for `makeRef2` since
  // the same code path implements both.

  return true;
}

static_assert(check_capture_val_to_ref());

constexpr bool check_capture_lref_to_lref() {
  int x = 5;
  // forwardRef: Copy `capture<V&>` bound as lref
  {
    capture<int&> ar{priv, forward_bind_wrapper(x)};
    check_one<capture<int&>, safe_alias::co_cleanup_safe_ref>([&]() -> auto& {
      return ar;
    });
    check_one<capture<const int&>, safe_alias::co_cleanup_safe_ref>(
        [&]() -> auto& { return std::as_const(ar); });
  }

  // forwardRef: Upgrade `after_cleanup_capture<V&>` bound as lref
  {
    auto lbind_x = forward_bind_wrapper(x);
    // NOLINTNEXTLINE(performance-move-const-arg)
    after_cleanup_capture<int&> ar{priv, std::move(lbind_x)};
    check_one_no_shared_cleanup<capture<int&>, safe_alias::after_cleanup_ref>(
        [&]() -> auto& { return ar; });
    check_one_no_shared_cleanup<
        capture<const int&>,
        safe_alias::after_cleanup_ref>([&]() -> auto& {
      return std::as_const(ar);
    });
  }

  // forwardRef: Do NOT upgrade `after_cleanup_capture<V&>`
  {
    after_cleanup_capture<int&> ar{priv, forward_bind_wrapper(x)};
    check_one_shared_cleanup<
        after_cleanup_capture<int&>,
        safe_alias::after_cleanup_ref>([&]() -> auto& { return ar; });
    check_one_shared_cleanup<
        after_cleanup_capture<const int&>,
        safe_alias::after_cleanup_ref>([&]() -> auto& {
      return std::as_const(ar);
    });
  }

  // forwardRef: Copy `co_cleanup_capture<V&>` bound as lref
  {
    HasCleanup cleanup;
    co_cleanup_capture<HasCleanup&> ar{priv, forward_bind_wrapper(cleanup)};
    check_one<
        co_cleanup_capture<HasCleanup&>,
        safe_alias::shared_cleanup,
        /*args_force_shared_cleanup*/ true>([&]() -> auto& { return ar; });
    check_one<
        co_cleanup_capture<const HasCleanup&>,
        safe_alias::shared_cleanup,
        /*args_force_shared_cleanup*/ true>([&]() -> auto& {
      return std::as_const(ar);
    });
  }

  // Manual test: binding `capture<V&&>` as an lvalue won't compile
  {
    // NOLINTNEXTLINE(performance-move-const-arg)
    capture<int&&> ar{priv, forward_bind_wrapper(std::move(x))};
#if 0
    check_one<capture<int&&>, safe_alias::co_cleanup_safe_ref>(
                    [&]() -> auto& { return ar; });
#endif
  }

  return true;
}

static_assert(check_capture_lref_to_lref());

constexpr bool check_capture_lref_to_rref() {
  int x = 5;

  // forwardRef: `capture<V&>` bound as rref -> `capture<V&&>
  check_one<capture<int&&>, safe_alias::co_cleanup_safe_ref>([&]() {
    return capture<int&>{priv, forward_bind_wrapper(x)};
  });

  // forwardRef: Upgrade `after_cleanup_capture<V&>` while moving
  check_one_no_shared_cleanup<capture<int&&>, safe_alias::after_cleanup_ref>(
      [&]() {
        return after_cleanup_capture<int&>{priv, forward_bind_wrapper(x)};
      });

  // forwardRef: Do NOT upgrade `after_cleanup_capture<V&>` while moving
  check_one_shared_cleanup<
      after_cleanup_capture<int&&>,
      safe_alias::after_cleanup_ref>([&]() {
    return after_cleanup_capture<int&>{priv, forward_bind_wrapper(x)};
  });

  // Manual test (set to 1 for compile error): Cannot move cleanup arg refs
#if 0
  HasCleanup cleanup;
  async_closure_safeties_and_bindings<async_closure_bindings_cfg{
      .force_outer_coro = false,
      .emit_now_task = false,
      .is_invoke_member = false}>(bind::args{
      co_cleanup_capture<HasCleanup&>{priv, forward_bind_wrapper(cleanup)}});
#endif

  return true;
}
static_assert(check_capture_lref_to_rref());

constexpr bool check_capture_rref_to_rref() {
  int x = 5;

  // forwardRef: `capture<V&>` bound as rref -> `capture<V&&>
  check_one<capture<int&&>, safe_alias::co_cleanup_safe_ref>([&]() {
    // NOLINTNEXTLINE(performance-move-const-arg)
    return capture<int&&>{priv, forward_bind_wrapper(std::move(x))};
  });

  // forwardRef: Upgrade `after_cleanup_capture<V&>` while moving
  check_one_no_shared_cleanup<capture<int&&>, safe_alias::after_cleanup_ref>(
      [&]() {
        return after_cleanup_capture<int&&>{
            priv,
            // NOLINTNEXTLINE(performance-move-const-arg)
            forward_bind_wrapper(std::move(x))};
      });

  // forwardRef: Do NOT upgrade `after_cleanup_capture<V&>` while moving
  check_one_shared_cleanup<
      after_cleanup_capture<int&&>,
      safe_alias::after_cleanup_ref>([&]() {
    return after_cleanup_capture<int&&>{
        priv,
        // NOLINTNEXTLINE(performance-move-const-arg)
        forward_bind_wrapper(std::move(x))};
  });

  return true;
}
static_assert(check_capture_rref_to_rref());

constexpr bool check_owned_capture_int() {
  check_one_no_shared_cleanup<
      async_closure_inner_stored_arg<capture<int>, bind_wrapper_t<int&&>>>(
      []() { return bind::capture(5); });
  // In this test, a `co_cleanup_capture` ref is passed as an argument, but
  // importantly, that doesn't force the closure to have an outer coro.
  check_one_shared_cleanup<async_closure_inner_stored_arg<
      after_cleanup_capture<int>,
      bind_wrapper_t<int&&>>>([]() { return bind::capture(5); });
  return true;
}

static_assert(check_owned_capture_int());

constexpr bool check_parent_capture_ref() {
  int x = 5;
  check_one_no_shared_cleanup<capture<const int&>, safe_alias::unsafe>([&]() {
    return bind::capture_const_ref{x};
  });
  check_one_no_shared_cleanup<capture<int&>, safe_alias::unsafe>([&]() {
    return bind::capture_mut_ref{x};
  });
  check_one_shared_cleanup<after_cleanup_capture<int&&>, safe_alias::unsafe>(
      [&]() { return bind::capture_mut_ref{std::move(x)}; });

  // Check multiple args together, including a stored argument eligible for
  // `after_cleanup` downgrade, and a ref eligible for an upgrade.  Ensures
  // that passing a "capture-by-ref" arg doesn't make a coro "shared cleanup".
  // This choice should be safe since `transform_binding` doesn't allow taking
  // references to `capture` types or `co_cleanup` types.  Contrariwise,
  // capture-by-ref wouldn't be very useful if it did force shared cleanup.
  constexpr async_closure_bindings_cfg Cfg{
      .force_outer_coro = false,
      .emit_now_task = false,
      .is_invoke_member = false};
  after_cleanup_capture<int> av{priv, forward_bind_wrapper(5)};
  using ActualTup = decltype(async_closure_safeties_and_bindings<Cfg>(
      bind::args{bind::capture{bind::const_ref{5}, 5}, av}));
  using ExpectedTup = lite_tuple::tuple<
      async_closure_arg_safety<
          safe_alias::unsafe,
          /*args_force_shared_cleanup*/ false>,
      lite_tuple::tuple<
          capture<const int&&>,
          async_closure_inner_stored_arg<capture<int>, bind_wrapper_t<int&&>>,
          capture<int&>>,
      std::nullptr_t>;
  static_assert(std::is_same_v<ActualTup, ExpectedTup>);

  return true;
}

static_assert(check_parent_capture_ref());

constexpr bool check_owned_cleanup_capture() {
  static_assert(
      std::is_same_v<
          decltype(async_closure_safeties_and_bindings<
                   async_closure_bindings_cfg{
                       .force_outer_coro = false,
                       .emit_now_task = false,
                       .is_invoke_member = false}>(
              bind::args{bind::capture_in_place<HasCleanup>()})),
          lite_tuple::tuple<
              // This is the safety from the point of view of the closure's
              // parent.  It does not matter that inside the closure, we have
              // a `shared_cleanup`-safety `co_cleanup_capture<HasCleanup&>`.
              async_closure_arg_safety<
                  safe_alias::maybe_value,
                  /*args_force_shared_cleanup*/ false>,
              lite_tuple::tuple<async_closure_outer_stored_arg<
                  co_cleanup_capture<HasCleanup>,
                  bind_wrapper_t<folly::bind::detail::in_place_args_maker<
                      detail::HasCleanup>>,
                  /*ArgI = */ 0,
                  /*Tag = */ folly::bind::ext::no_tag_t{}>>,
              std::nullptr_t>>);
  return true;
}

static_assert(check_owned_cleanup_capture());

constexpr bool check_emit_now_task_blocks_ref_upgrade() {
  int x = 7;
  after_cleanup_capture<int&> cr{priv, forward_bind_wrapper(x)};
  static_assert(
      std::is_same_v<
          decltype(async_closure_safeties_and_bindings<
                   async_closure_bindings_cfg{
                       .force_outer_coro = false,
                       .emit_now_task = false,
                       .is_invoke_member = false}>(bind::args{cr})),
          lite_tuple::tuple<
              // ref upgrades don't affect the parent's measurement...
              async_closure_arg_safety<
                  safe_alias::after_cleanup_ref,
                  /*args_force_shared_cleanup*/ false>,
              // ...but the child sees a more-safe `capture<int&>`
              lite_tuple::tuple<capture<int&>>,
              std::nullptr_t>>);

  // Compared to the above, we only change `.emit_now_task = true`...
  static_assert(
      std::is_same_v<
          decltype(async_closure_safeties_and_bindings<
                   async_closure_bindings_cfg{
                       .force_outer_coro = false,
                       .emit_now_task = true, // NOTE: `false` ABOVE
                       .is_invoke_member = false}>(bind::args{cr})),
          lite_tuple::tuple<
              async_closure_arg_safety<
                  safe_alias::after_cleanup_ref,
                  /*args_force_shared_cleanup*/ false>,
              // ... we upgrade refs here (safe inner coro),
              lite_tuple::tuple<capture<int&>>,
              // ... but not here (unsafe inner coro).
              lite_tuple::tuple<after_cleanup_capture<int&>>>>);

  // Still `.emit_now_task = true`, but now with the addition of
  // a new arg that forces shared-cleanup behavior...
  static_assert(
      std::is_same_v<
          decltype(async_closure_safeties_and_bindings<
                   async_closure_bindings_cfg{
                       .force_outer_coro = false,
                       .emit_now_task = true,
                       .is_invoke_member = false}>(bind::args{
              cr,
              // NOTE: NEW ARGUMENT
              std::declval<co_cleanup_capture<HasCleanup&>&>()})),
          lite_tuple::tuple<
              async_closure_arg_safety<
                  safe_alias::shared_cleanup,
                  /*args_force_shared_cleanup*/ true>,
              lite_tuple::tuple<
                  // ... thanks to the arg, we can never upgrade refs,
                  after_cleanup_capture<int&>,
                  co_cleanup_capture<folly::coro::detail::HasCleanup&>>,
              // ... so the third entry is unused.
              std::nullptr_t>>);

  return true;
}

static_assert(check_emit_now_task_blocks_ref_upgrade());

constexpr bool check_force_outer_coro() {
  static_assert(
      std::is_same_v<
          decltype(async_closure_safeties_and_bindings<
                   async_closure_bindings_cfg{
                       .force_outer_coro = false,
                       .emit_now_task = false,
                       .is_invoke_member = false}>(
              bind::args{bind::capture(5)})),
          lite_tuple::tuple<
              async_closure_arg_safety<
                  safe_alias::maybe_value,
                  /*args_force_shared_cleanup*/ false>,
              // inner <=> no outer coro
              lite_tuple::tuple<async_closure_inner_stored_arg<
                  capture<int>,
                  bind_wrapper_t<int&&>>>,
              std::nullptr_t>>);
  static_assert(
      std::is_same_v<
          decltype(async_closure_safeties_and_bindings<
                   async_closure_bindings_cfg{
                       .force_outer_coro = true, // this changed...
                       .emit_now_task = false,
                       .is_invoke_member = false}>(
              bind::args{bind::capture(5)})),
          lite_tuple::tuple<
              async_closure_arg_safety<
                  safe_alias::maybe_value,
                  /*args_force_shared_cleanup*/ false>,
              // ... outer <=> no outer coro
              lite_tuple::tuple<async_closure_outer_stored_arg<
                  capture<int>,
                  bind_wrapper_t<int&&>,
                  /*ArgI = */ 0,
                  /*Tag = */ folly::bind::ext::no_tag_t{}>>,
              std::nullptr_t>>);
  return true;
}

static_assert(check_force_outer_coro());

constexpr bool check_is_invoke_member_implicit_capture() {
  // `is_invoke_member = true` implicitly captures the 1st arg rvalue
  static_assert(
      std::is_same_v<
          decltype(async_closure_safeties_and_bindings<
                   async_closure_bindings_cfg{
                       // Without the outer coro, a static assert would fire.
                       // Rationale: we need a stable address for the implicit
                       // object parameter, and moving it into the inner coro
                       // cannot provide that.
                       .force_outer_coro = true,
                       .emit_now_task = false,
                       .is_invoke_member = true}>(
              bind::args{MoveMe{}, MoveMe{}})),
          lite_tuple::tuple<
              async_closure_arg_safety<
                  safe_alias::maybe_value,
                  /*args_force_shared_cleanup*/ false>,
              lite_tuple::tuple<
                  async_closure_outer_stored_arg<
                      capture<MoveMe>,
                      bind_wrapper_t<MoveMe&&>,
                      /*ArgI = */ 0,
                      /*Tag = */ folly::bind::ext::no_tag_t{}>,
                  // The second arg is NOT implicitly captured
                  async_closure_regular_arg<MoveMe, bind_wrapper_t<MoveMe&&>>>,
              std::nullptr_t>>);
  // Same as above, but with `is_invoke_member = false`
  static_assert(
      std::is_same_v<
          decltype(async_closure_safeties_and_bindings<
                   async_closure_bindings_cfg{
                       .force_outer_coro = true,
                       .emit_now_task = false,
                       .is_invoke_member = false}>(
              bind::args{MoveMe{}, MoveMe{}})),
          lite_tuple::tuple<
              async_closure_arg_safety<
                  safe_alias::maybe_value,
                  /*args_force_shared_cleanup*/ false>,
              lite_tuple::tuple<
                  // Neither arg is implicitly captured
                  async_closure_regular_arg<MoveMe, bind_wrapper_t<MoveMe&&>>,
                  async_closure_regular_arg<MoveMe, bind_wrapper_t<MoveMe&&>>>,
              std::nullptr_t>>);
  return true;
}

static_assert(check_is_invoke_member_implicit_capture());

} // namespace folly::coro::detail

#endif
