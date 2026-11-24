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

#include <folly/coro/safe/Captures.h>
#include <folly/lang/bind/Bind.h>
#include <folly/portability/GTest.h>

#ifdef FOLLY_MOVABLE_AND_DEEP_CONST_LREF_COPYABLE
static_assert(false, "Leaked FOLLY_MOVABLE_AND_DEEP_CONST_LREF_COPYABLE");
#endif

#if FOLLY_HAS_COROUTINES

namespace folly::coro::detail {

struct SimpleCleanup : NonCopyableNonMovable {
  void co_cleanup(async_closure_private_t) {}
  int x() & { return 1001; }
  int x() const& { return 2002; }
  int x() && { return 3003; }
  void canMutate() {}
};

decltype(auto) get_address(const auto& v) {
  return &v;
}

template <typename TA, typename TB>
void check_capture_same_address(TA&& a, TB&& b) {
  EXPECT_EQ(
      get_address(*std::forward<TA>(a)), get_address(*std::forward<TB>(b)));
}

struct CapturesTest : testing::Test {
  // Required to access friend-only ctor of `capture`s.
  static constexpr capture_private_t arg_priv{};

  template <typename ArgT>
  static constexpr auto make(auto&& arg) {
    return ArgT{
        arg_priv, forward_bind_wrapper(std::forward<decltype(arg)>(arg))};
  }

  template <typename ArgT>
  auto make_co_cleanup(auto&&... args) {
    return co_cleanup_capture<ArgT>{
        arg_priv,
        unsafe_tuple_to_bind_wrapper(
            bind::in_place<ArgT>(std::forward<decltype(args)>(args)...)
                .unsafe_tuple_to_bind())};
  }

  template <typename Arg>
  auto shared_cleanup_ref(Arg&& arg) {
    return std::forward<Arg>(arg).template to_capture_ref<true>(arg_priv);
  }
  template <typename Arg>
  auto independent_cleanup_ref(Arg&& arg) {
    return std::forward<Arg>(arg).template to_capture_ref<false>(arg_priv);
  }

  template <template <typename> class RefArg = capture, typename T = int>
  void check_ref_from_mutable(int expected, auto a, auto make_ref) {
    {
      auto ar = make_ref(a);
      static_assert(std::is_same_v<decltype(ar), RefArg<T&>>);
      EXPECT_EQ(expected, *ar);
      *a += 10;
      expected += 10;

      auto ar2 = make_ref(ar);
      static_assert(std::is_same_v<decltype(ar2), RefArg<T&>>);
      EXPECT_EQ(expected, *ar2);

      // Future: How to concisely test that just `T&` won't compile?
      auto ar3 = make_ref(std::as_const(ar2));
      static_assert(std::is_same_v<decltype(ar3), RefArg<const T&>>);
      EXPECT_EQ(expected, *ar3);

      // Future: How to test here (and below) that `std::move` is required?
      auto ar4 = make_ref(std::move(ar2));
      static_assert(std::is_same_v<decltype(ar4), RefArg<T&&>>);
      EXPECT_EQ(expected, *std::move(ar4));
    }
    {
      auto ar = make_ref(std::move(a));
      static_assert(std::is_same_v<decltype(ar), RefArg<T&&>>);
      EXPECT_EQ(expected, *std::move(ar));
      // NOLINTNEXTLINE(bugprone-use-after-move)
      *a -= 10;
      expected -= 10;

      // NOLINTNEXTLINE(bugprone-use-after-move)
      auto ar2 = make_ref(std::move(ar));
      static_assert(std::is_same_v<decltype(ar2), RefArg<T&&>>);
      EXPECT_EQ(expected, *std::move(ar2));
    }
  }

  template <template <typename> class RefArg = capture, typename T = int>
  void check_ref_from_const(int expected, auto a, auto make_ref) {
    {
      auto ar = make_ref(a);
      static_assert(std::is_same_v<decltype(ar), RefArg<const T&>>);
      check_capture_same_address(a, ar);

      auto ar2 = make_ref(ar);
      static_assert(std::is_same_v<decltype(ar2), RefArg<const T&>>);
      check_capture_same_address(a, ar2);

      auto ar3 = make_ref(std::move(ar2));
      static_assert(std::is_same_v<decltype(ar3), RefArg<const T&&>>);
      EXPECT_EQ(expected, *std::move(ar3));
    }
    {
      auto ar = make_ref(std::move(a));
      static_assert(std::is_same_v<decltype(ar), RefArg<const T&&>>);

      auto ar2 = make_ref(std::move(ar));
      static_assert(std::is_same_v<decltype(ar2), RefArg<const T&&>>);
      EXPECT_EQ(expected, *std::move(ar2));
    }
  }

  template <
      typename T = SimpleCleanup,
      template <typename> class RefArg = co_cleanup_capture>
  void check_ref_from_cleanup(auto a, auto make_ref) {
    auto ar = make_ref(a);
    static_assert(std::is_same_v<decltype(ar), RefArg<T&>>);
    check_capture_same_address(a, ar);
    if constexpr (!std::is_const_v<T>) {
      a->canMutate();
    }

    auto ar2 = make_ref(ar);
    static_assert(std::is_same_v<decltype(ar2), RefArg<T&>>);
    check_capture_same_address(a, ar2);

    // Future: How to concisely test that just `T&` won't compile?
    auto ar3 = make_ref(std::as_const(ar2));
    static_assert(std::is_same_v<decltype(ar3), RefArg<const T&>>);
    check_capture_same_address(a, ar3);
  }

  template <
      template <typename> class RefFromAfterCleanup,
      template <typename> class IndirectRefFromAfterCleanup>
  void check_to_capture_ref(auto make_ref_fn) {
    check_ref_from_mutable(5, make<capture<int>>(5), make_ref_fn);
    check_ref_from_mutable(5, make<capture_heap<int>>(5), make_ref_fn);
    check_ref_from_mutable<capture_indirect, std::unique_ptr<int>>(
        5,
        make<capture_indirect<std::unique_ptr<int>>>(std::make_unique<int>(5)),
        make_ref_fn);
    check_ref_from_cleanup(make_co_cleanup<SimpleCleanup>(), make_ref_fn);

    // Same, but with a `const` type -- no mutability assertions.
    check_ref_from_const(7, make<capture<const int>>(7), make_ref_fn);
    check_ref_from_const(7, make<capture_heap<const int>>(7), make_ref_fn);
    check_ref_from_const<capture_indirect, std::unique_ptr<int>>(
        7,
        make<capture_indirect<const std::unique_ptr<int>>>(
            std::make_unique<int>(7)),
        make_ref_fn);
    check_ref_from_cleanup<const SimpleCleanup>(
        make_co_cleanup<const SimpleCleanup>(), make_ref_fn);

    // Repeat the above blocks, checking whether we shed `after_cleanup_ref_`
    // from the ref.  There is no `after_cleanup_ref_co_cleanup_capture`, of
    // course.

    check_ref_from_mutable<RefFromAfterCleanup>(
        5, make<after_cleanup_capture<int>>(5), make_ref_fn);
    check_ref_from_mutable<RefFromAfterCleanup>(
        5, make<after_cleanup_capture_heap<int>>(5), make_ref_fn);
    check_ref_from_mutable<IndirectRefFromAfterCleanup, std::unique_ptr<int>>(
        5,
        make<after_cleanup_capture_indirect<std::unique_ptr<int>>>(
            std::make_unique<int>(5)),
        make_ref_fn);

    check_ref_from_const<RefFromAfterCleanup>(
        7, make<after_cleanup_capture<const int>>(7), make_ref_fn);
    check_ref_from_const<RefFromAfterCleanup>(
        7, make<after_cleanup_capture_heap<const int>>(7), make_ref_fn);
    check_ref_from_const<IndirectRefFromAfterCleanup, std::unique_ptr<int>>(
        7,
        make<after_cleanup_capture_indirect<const std::unique_ptr<int>>>(
            std::make_unique<int>(7)),
        make_ref_fn);
  }

  struct TestStruct {
    int member_;
  };

  // Check that member access through capture has expected value category.
  // Crucially, we use `(*x).` instead of `x->` since the latter returns a
  // pointer, discarding value category.
  template <
      typename InnerType,
      typename ExpectedRvalMemberType,
      typename ExpectedLvalMemberType = void>
  static constexpr void check_member_access() {
    using Capture = capture<InnerType>;

    TestStruct obj{};
    auto cap = make<Capture>(obj);
    static_assert(std::is_same_v<Capture, decltype(cap)>);

    static_assert(
        std::is_same_v<
            decltype(((*static_cast<Capture&&>(cap)).member_)),
            ExpectedRvalMemberType>);

    // C++ language limitation: `operator->` returns a pointer, so it always
    // gives lvalue access regardless of value category
    static_assert(
        std::is_same_v<
            decltype((static_cast<Capture&&>(cap)->member_)),
            std::remove_reference_t<ExpectedRvalMemberType>&>);

    // Test lvalue access for `capture<V>` and `capture<V&>`.  `capture<V&&>`
    // may only be accessed by-rvalue.
    if constexpr (!std::is_rvalue_reference_v<InnerType>) {
      static_assert(
          std::is_same_v<decltype(((*cap).member_)), ExpectedLvalMemberType>);
      static_assert(
          std::is_same_v<
              decltype(((*static_cast<Capture&>(cap)).member_)),
              ExpectedLvalMemberType>);
      static_assert(
          std::is_same_v<
              decltype((static_cast<Capture&>(cap)->member_)),
              ExpectedLvalMemberType&>);
    }
  }

  // Test the value category behavior of member access through capture-refs.
  static constexpr bool test_member_value_category() {
    // capture<V> (owned value) gives lvalue member for lvalue access, and
    // rvalue members for rvalue access.
    check_member_access<TestStruct, int&&, int&>();
    check_member_access<const TestStruct, const int&&, const int&>();

    // capture<V&> always gives lval members, even when accessed by rvalue.
    check_member_access<TestStruct&, int&, int&>();
    check_member_access<const TestStruct&, const int&, const int&>();

    // capture<V&&> is only accessed by rvalue.
    check_member_access<TestStruct&&, int&&>();
    check_member_access<const TestStruct&&, const int&&>();

    return true;
  }
};

static_assert(CapturesTest::test_member_value_category());

TEST_F(CapturesTest, indirect_getUnderlyingUnsafe) {
  auto ci = make<capture_indirect<std::unique_ptr<short>>>(
      std::make_unique<short>(37));
  auto x = std::move(ci).get_underlying_unsafe();
  static_assert(std::is_same_v<decltype(x), std::unique_ptr<short>>);
  EXPECT_EQ(37, *x);
}

TEST_F(CapturesTest, to_capture_ref_sharedCleanup) {
  // We set `SharedCleanup == true`, so an input arg type with a
  // `after_cleanup_ref_` prefix will retain that prefix on the ref.
  check_to_capture_ref<after_cleanup_capture, after_cleanup_capture_indirect>(
      [&](auto&& arg) {
        return shared_cleanup_ref(std::forward<decltype(arg)>(arg));
      });
}

TEST_F(CapturesTest, to_capture_ref_independentCleanup) {
  // "Upgrade" behavior: We set `SharedCleanup == false`, so all arg types
  // emit `capture` refs, even when the input was `after_cleanup_ref_`.
  check_to_capture_ref<capture, capture_indirect>([&](auto&& arg) {
    return independent_cleanup_ref(std::forward<decltype(arg)>(arg));
  });
}

TEST_F(CapturesTest, capture_implicitRefConversion) {
  // Implicit conversion to capture refs parallels
  // `to_capture_ref_sharedCleanup` -- we must not upgrade
  // `after_cleanup_ref_async_arc*` to non-`after_cleanup_ref`, since we're not
  // creating an independent scope for the new capture ref.
  check_to_capture_ref<after_cleanup_capture, after_cleanup_capture_indirect>(
      [&](auto&& arg) {
        return capture_ref_conversion_t<decltype(arg)>{
            std::forward<decltype(arg)>(arg)};
      });

  // Sample no-abstraction checks -- `check_to_capture_ref` has more coverage
  {
    auto a = make<capture<int>>(5);
    capture<const int&> aclr = std::as_const(a);
    check_capture_same_address(a, aclr);
    auto arr = capture<int&&>{std::move(a)};
    check_capture_same_address(a, std::move(arr));
    // Explicitly converting an rref into an lref is NOT covered by
    // `check_to_capture_ref` because that logic is not part of
    // async_closure binding conversions.
    // NOLINTNEXTLINE(bugprone-use-after-move)
    auto alr = capture<int&>{std::move(arr)};
    check_capture_same_address(a, std::move(alr));
  }
  {
    auto a = make<capture<const int>>(5);
    capture<const int&> aclr = std::as_const(a);
    check_capture_same_address(a, std::move(aclr));
    auto acrr = capture<const int&&>{capture<const int&>{std::as_const(a)}};
    check_capture_same_address(a, std::move(acrr));
  }
  {
    auto a = make_co_cleanup<SimpleCleanup>();
    co_cleanup_capture<SimpleCleanup&> ar = a;
    check_capture_same_address(a, std::move(ar));
  }

  // Check `capture_ref_conversion_t` which is intended for finding the
  // reference type, to which an capture instance can be implicitly
  // converted.

  // Simple mutable refs
  static_assert(
      std::is_same_v<capture_ref_conversion_t<capture<int>&>, capture<int&>>);
  static_assert(
      std::is_same_v<capture_ref_conversion_t<capture<int>>, capture<int&&>>);
  static_assert(
      std::is_same_v<capture_ref_conversion_t<capture<int>&&>, capture<int&&>>);

  // Same, with a `const` wrapped type, ensuring we don't strip `const`
  static_assert(
      std::is_same_v<
          capture_ref_conversion_t<capture<const int>&>,
          capture<const int&>>);
  static_assert(
      std::is_same_v<
          capture_ref_conversion_t<capture<const int>>,
          capture<const int&&>>);
  static_assert(
      std::is_same_v<
          capture_ref_conversion_t<capture<const int>&&>,
          capture<const int&&>>);

  // `const` from the wrapper moves to the wrapped type
  static_assert(
      std::is_same_v<
          capture_ref_conversion_t<const capture<int>&>,
          capture<const int&>>);
  static_assert(
      std::is_same_v<
          capture_ref_conversion_t<const capture<int>>,
          capture<const int&&>>);

  // The outer reference type clobbers the inner one.  We DON'T want the
  // `lref&&`-remains-an-lref reference collapsing rule here, since it seems
  // desirable for this to work:
  //   capture<T&&> myRref = std::move(myLref);
  static_assert(
      std::is_same_v<capture_ref_conversion_t<capture<int&&>&>, capture<int&>>);
  static_assert(
      std::is_same_v<capture_ref_conversion_t<capture<int&>&>, capture<int&>>);
  static_assert(
      std::
          is_same_v<capture_ref_conversion_t<capture<int&>&&>, capture<int&&>>);
  static_assert(
      std::is_same_v<
          capture_ref_conversion_t<capture<int&&>&&>,
          capture<int&&>>);
}

TEST_F(CapturesTest, make_in_place) {
  // A little redundant with `make_co_cleanup`, but isolated
  capture<std::string> cap{
      arg_priv,
      unsafe_tuple_to_bind_wrapper(
          bind::in_place<std::string>("hi").unsafe_tuple_to_bind())};
}

TEST_F(CapturesTest, noCustomDereference) {
  static_assert(has_async_closure_co_cleanup<SimpleCleanup>);

  auto sc = make_co_cleanup<SimpleCleanup>();
  EXPECT_EQ(1001, sc->x());
  EXPECT_EQ(2002, std::as_const(sc)->x());
  {
    auto sc2 = make_co_cleanup<SimpleCleanup>();
    EXPECT_EQ(3003, (*std::move(sc2)).x());
  }
  {
    auto sc2 = make_co_cleanup<SimpleCleanup>();
    // Note: Unfortunately, since `->` returns a plain pointer in the
    // absence of `capture_proxy`, `std::move(sc)->` acts like `sc->`.
    EXPECT_EQ(1001, std::move(sc2)->x());
  }

  auto rsc = restricted_co_cleanup_capture<SimpleCleanup&>{
      arg_priv, forward_bind_wrapper(*sc)};
#if 0 // Manual test: won't compile due to missing
      // `capture_restricted_proxy`
  static_assert(!requires { rsc->x(); });
#endif
}

struct CustomDerefCleanupRef : NonCopyableNonMovable {
  explicit CustomDerefCleanupRef(int y) : y_(y) {}
  auto operator->() { return static_cast<CustomDerefCleanupRef*>(this); }
  int y_;
};

struct CustomDerefCleanup : NonCopyableNonMovable {
  void co_cleanup(async_closure_private_t) {}
  template <
      ext::capture_proxy_kind Kind,
      ext::const_or_not<CustomDerefCleanup> T>
  friend auto capture_proxy(ext::capture_proxy_tag<Kind>, T&) {
    if constexpr (Kind == ext::capture_proxy_kind::lval_ref) {
      return CustomDerefCleanupRef{101 + 1000 * std::is_const_v<T>};
    } else if constexpr (Kind == ext::capture_proxy_kind::lval_ptr) {
      return CustomDerefCleanupRef{202 + 1000 * std::is_const_v<T>};
    } else if constexpr (Kind == ext::capture_proxy_kind::rval_ref) {
      return CustomDerefCleanupRef{303 + 1000 * std::is_const_v<T>};
    } else if constexpr (Kind == ext::capture_proxy_kind::rval_ptr) {
      return CustomDerefCleanupRef{404 + 1000 * std::is_const_v<T>};
    } else {
      static_assert(false);
    }
  }
  template <
      ext::capture_proxy_kind Kind,
      ext::const_or_not<CustomDerefCleanup> T>
  friend auto capture_restricted_proxy(ext::capture_proxy_tag<Kind>, T&) {
    if constexpr (Kind == ext::capture_proxy_kind::lval_ref) {
      return CustomDerefCleanupRef{11 + 100 * std::is_const_v<T>};
    } else if constexpr (Kind == ext::capture_proxy_kind::lval_ptr) {
      return CustomDerefCleanupRef{22 + 100 * std::is_const_v<T>};
    } else if constexpr (Kind == ext::capture_proxy_kind::rval_ref) {
      return CustomDerefCleanupRef{33 + 100 * std::is_const_v<T>};
    } else if constexpr (Kind == ext::capture_proxy_kind::rval_ptr) {
      return CustomDerefCleanupRef{44 + 100 * std::is_const_v<T>};
    } else {
      static_assert(false);
    }
  }
};

TEST_F(CapturesTest, customDereference) {
  static_assert(has_async_closure_co_cleanup<CustomDerefCleanup>);

  auto c = make_co_cleanup<CustomDerefCleanup>();

  EXPECT_EQ(101, (*c).y_);
  EXPECT_EQ(202, c->y_);
  EXPECT_EQ(404, shared_cleanup_ref(c)->y_);

  EXPECT_EQ(1101, (*std::as_const(c)).y_);
  EXPECT_EQ(1202, std::as_const(c)->y_);
  EXPECT_EQ(1404, shared_cleanup_ref(std::as_const(c))->y_);

  CustomDerefCleanup c2;
  auto rc = restricted_co_cleanup_capture<CustomDerefCleanup&>{
      arg_priv, forward_bind_wrapper(c2)};

  EXPECT_EQ(11, (*rc).y_);
  EXPECT_EQ(22, rc->y_);
  EXPECT_EQ(33, (*std::move(rc)).y_);

  EXPECT_EQ(111, (*std::as_const(rc)).y_);
  EXPECT_EQ(122, std::as_const(rc)->y_);
  // NOLINTNEXTLINE(performance-move-const-arg)
  EXPECT_EQ(133, (*std::move(std::as_const(rc))).y_);
}

TEST_F(CapturesTest, copyLValueRef) {
  int n = 5;
  // Copy non-const to non-const
  {
    auto src = make<capture<int&>>(n);
    capture<int&> dst{src}; // copy ctor
    dst = src; // copy assignment
  }

  // Copy to const from non-const OR from const
  {
    auto src = make<capture<int&>>(n);
    capture<const int&> dst{src};
    dst = src;
  }
  {
    auto src = make<capture<int&>>(n);
    capture<const int&> dst{std::as_const(src)};
    dst = std::as_const(src);
  }

  // CANNOT copy from `const capture<int&>` to `capture<int&>` because we
  // want this `capture` to have "deep const" semantics -- a `const` wrapper
  // should protect the underlying data from changes.
#if 0 // Manual test -- should fail with a "no copy ctor" error.
  auto src = make<capture<int&>>(n);
  capture<int&> dst{std::as_const(src)};
#endif
  // The asserts approximate the `#if 0` manual test that won't compile.
  // Check both "true" and "false" cases to ensure the test itself is correct.
  static_assert(
      std::is_constructible_v<capture<const int&>, const capture<int>&>);
  static_assert(!std::is_constructible_v<capture<int&>, const capture<int>&>);
#if 0 // Manual test -- should fail with a "no copy assignment" error.
  auto src = make<capture<int&>>(n);
  capture<int&> dst{src}; // same as "copy non-const to non-const" above
  dst = std::as_const(src); // fails here
#endif
  static_assert(std::is_assignable_v<capture<const int&>, const capture<int>&>);
  static_assert(!std::is_assignable_v<capture<int&>, const capture<int>&>);
}

TEST_F(CapturesTest, moveLValueRef) {
  // Move non-const& to non-const&
  {
    int n = 5;
    auto src = make<capture<int&>>(n);
    capture<int&> dst{std::move(src)}; // move ctor
    (void)dst;
  }
  {
    int n = 5;
    auto src = make<capture<int&>>(n);
    capture<int&> dst{make<capture<int&>>(n)};
    dst = std::move(src); // move assignment
  }

  // Move non-const& to non-const&&
  {
    int n = 5;
    auto src = make<capture<int&>>(n);
    capture<int&&> dst{std::move(src)}; // move ctor
    (void)dst;
  }
  {
    int n = 5;
    auto src = make<capture<int&>>(n);
    capture<int&&> dst{make<capture<int&&>>(std::move(n))};
    dst = capture<int&&>{std::move(src)}; // move assignment
  }

  // Move non-const& to const&
  {
    int n = 5;
    auto src = make<capture<int&>>(n);
    capture<const int&> dst{std::move(src)}; // move ctor
    (void)dst;
  }
  {
    int n = 5;
    auto src = make<capture<int&>>(n);
    capture<const int&> dst{make<capture<const int&>>(n)};
    dst = std::move(src); // move assignment
  }

  // Like the CANNOT test in `copyLValueRef`, check we don't shed `const`
  static_assert(
      std::is_constructible_v<capture<const int&>, const capture<int>&&>);
  static_assert(!std::is_constructible_v<capture<int&>, const capture<int>&&>);

  // Maybe future: support "move non-const& to const&&".  Currently not done
  // because of its low utility.  Also add a CANNOT shed const test.
}

TEST_F(CapturesTest, onlyMoveRValueRef) {
  // Move non-const to non-const
  {
    int n = 5;
    auto src = make<capture<int&&>>(std::move(n));
    capture<int&&> dst{std::move(src)}; // move ctor
    (void)dst;
  }
  {
    int n1 = 5, n2 = 6;
    auto src = make<capture<int&&>>(std::move(n1));
    capture<int&&> dst{make<capture<int&&>>(std::move(n2))};
    dst = std::move(src); // move assignment
  }

  // Future: Maybe add a move to `const &&` from non-const.  Right now I'm
  // missing an overload for this due to its low utility.  Also add a CANNOT
  // shed const test.
}

} // namespace folly::coro::detail

#endif
