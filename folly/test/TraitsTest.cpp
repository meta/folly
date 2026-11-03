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

#include <folly/Traits.h>

#include <cstring>
#include <list>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <folly/ScopeGuard.h>
#include <folly/portability/GTest.h>

using namespace folly;
using namespace std;

struct type_identity_test {
  template <typename A>
  static A deduce(A, std::type_identity_t<A>);
};

//  Guards the non-deduced-context property that makeFuture/makeSemiFuture rely
//  on to pin the error type to the explicit template argument.
TEST(Traits, type_identity) {
  EXPECT_TRUE(( //
      std::is_same_v<int, decltype(type_identity_test::deduce(0, '\0'))>));
}

namespace {

struct T1 {}; // old-style IsRelocatable, below
struct T2 {}; // old-style IsRelocatable, below
struct T3 {
  using IsRelocatable = std::true_type;
};
struct T5 : T3 {};

struct F1 {};
struct F2 {
  using IsRelocatable = int;
};
struct F3 : T3 {
  using IsRelocatable = std::false_type;
};
struct F4 : T1 {};

struct T6 {
  T6(const T6&) {}
  T6& operator=(const T6&) { return *this; }
};
static_assert(!std::is_trivially_copyable_v<T6>);

template <class>
struct A {};
struct B {};

} // namespace

TEST(Traits, index_iterals) {
  using namespace folly::literals;
  constexpr std::integral_constant val = 17_uzic;
  EXPECT_EQ(17, val);
}

namespace folly {
template <>
struct IsRelocatable<T1> : std::true_type {};
template <>
FOLLY_ASSUME_RELOCATABLE(T2);
template <>
FOLLY_ASSUME_RELOCATABLE(T6);
} // namespace folly

TEST(Traits, scalars) {
  EXPECT_TRUE(IsRelocatable<int>::value);
  EXPECT_TRUE(IsRelocatable<bool>::value);
  EXPECT_TRUE(IsRelocatable<double>::value);
  EXPECT_TRUE(IsRelocatable<void*>::value);
}

TEST(Traits, containers) {
  EXPECT_FALSE(IsRelocatable<vector<F1>>::value);
  EXPECT_TRUE((IsRelocatable<pair<F1, F1>>::value));
  EXPECT_TRUE((IsRelocatable<pair<T1, T2>>::value));
  EXPECT_TRUE((IsRelocatable<pair<const T1, T2>>::value));
  EXPECT_TRUE((IsRelocatable<pair<const T6, T2>>::value));
  EXPECT_FALSE((IsRelocatable<pair<const vector<F1>, T2>>::value));
}

TEST(Traits, constQualified) {
  EXPECT_TRUE(IsRelocatable<const int>::value);
  EXPECT_TRUE(IsRelocatable<const T1>::value);
  EXPECT_TRUE(IsRelocatable<const T3>::value);
  EXPECT_TRUE(IsRelocatable<const T6>::value);
  EXPECT_FALSE(IsRelocatable<const F3>::value);
  EXPECT_FALSE(IsRelocatable<const vector<F1>>::value);
}

TEST(Traits, original) {
  EXPECT_TRUE(IsRelocatable<T1>::value);
  EXPECT_TRUE(IsRelocatable<T2>::value);
}

TEST(Traits, typedefd) {
  EXPECT_TRUE(IsRelocatable<T3>::value);
  EXPECT_TRUE(IsRelocatable<T5>::value);
  EXPECT_FALSE(IsRelocatable<F2>::value);
  EXPECT_FALSE(IsRelocatable<F3>::value);
}

TEST(Traits, unset) {
  EXPECT_TRUE(IsRelocatable<F1>::value);
  EXPECT_TRUE(IsRelocatable<F4>::value);
}

TEST(Traits, zeroInit) {
  // S1 is both trivially default-constructible and trivially
  // value-initializable. S2 is neither. S3 is trivially default-constructible
  // but not trivially value-initializable.
  struct S1 {
    int i_;
  };
  struct S2 {
    int i_ = 42;
  };
  struct S3 {
    int S1::* mp_;
  };

  EXPECT_TRUE(IsZeroInitializable<int>::value);
  EXPECT_TRUE(IsZeroInitializable<int*>::value);
  EXPECT_FALSE(IsZeroInitializable<vector<int>>::value);
  EXPECT_FALSE(IsZeroInitializable<S2>::value);
  EXPECT_FALSE(IsZeroInitializable<int S1::*>::value); // Itanium
  EXPECT_FALSE(IsZeroInitializable<S3>::value); // Itanium

  // TODO: S1 actually can be trivially zero-initialized, but
  // there's no portable way to distinguish it from S3, which can't.
  EXPECT_FALSE(IsZeroInitializable<S1>::value);
}

template <bool V>
struct Cond {
  template <typename K = std::string>
  static auto fun_std(std::conditional_t<V, K, std::string>&& arg) {
    return std::is_same<folly::remove_cvref_t<decltype(arg)>, std::string>{};
  }
  template <typename K = std::string>
  static auto fun_folly(folly::conditional_t<V, K, std::string>&& arg) {
    return std::is_same<folly::remove_cvref_t<decltype(arg)>, std::string>{};
  }
};

TEST(Traits, conditional) {
  using folly::conditional_t;
  EXPECT_TRUE((std::is_same<conditional_t<false, char, int>, int>::value));
  EXPECT_TRUE((std::is_same<conditional_t<true, char, int>, char>::value));

  EXPECT_TRUE(Cond<false>::fun_std("hello"));
  EXPECT_TRUE(Cond<true>::fun_std("hello"));
  EXPECT_TRUE(Cond<false>::fun_folly("hello"));
  EXPECT_FALSE(Cond<true>::fun_folly("hello"));
}

template <typename T>
using value_type_t = typename T::value_type;

template <typename T>
using value_type_or_self_t = folly::conditional_apply_t<requires {
  typename T::value_type;
}, value_type_t, std::type_identity_t, T>;

TEST(Traits, conditionalApply) {
  using folly::conditional_apply_t;
  EXPECT_TRUE((std::is_same_v<
               conditional_apply_t<true, std::vector, std::list, int>,
               std::vector<int>>));
  EXPECT_TRUE((std::is_same_v<
               conditional_apply_t<false, std::vector, std::list, int>,
               std::list<int>>));
  EXPECT_TRUE((std::is_same_v<
               conditional_apply_t<true, std::pair, std::tuple, int, char>,
               std::pair<int, char>>));
  EXPECT_TRUE((std::is_same_v<
               conditional_apply_t<false, std::pair, std::tuple>,
               std::tuple<>>));

  EXPECT_TRUE((std::is_same_v<value_type_or_self_t<std::vector<char>>, char>));
  EXPECT_TRUE((std::is_same_v<value_type_or_self_t<int>, int>));
}

TEST(Traits, isNegative) {
  EXPECT_TRUE(folly::is_negative(-1));
  EXPECT_FALSE(folly::is_negative(0));
  EXPECT_FALSE(folly::is_negative(1));
  EXPECT_FALSE(folly::is_negative(0u));
  EXPECT_FALSE(folly::is_negative(1u));

  EXPECT_TRUE(folly::is_non_positive(-1));
  EXPECT_TRUE(folly::is_non_positive(0));
  EXPECT_FALSE(folly::is_non_positive(1));
  EXPECT_TRUE(folly::is_non_positive(0u));
  EXPECT_FALSE(folly::is_non_positive(1u));
}

TEST(Traits, relational) {
  // We test, especially, the edge cases to make sure we don't
  // trip -Wtautological-comparisons

  EXPECT_FALSE((folly::less_than<uint8_t, 0u, uint8_t>(0u)));
  EXPECT_FALSE((folly::less_than<uint8_t, 0u, uint8_t>(254u)));
  EXPECT_FALSE((folly::less_than<uint8_t, 255u, uint8_t>(255u)));
  EXPECT_TRUE((folly::less_than<uint8_t, 255u, uint8_t>(254u)));

  // Making sure signed to unsigned comparisons are not truncated.
  EXPECT_TRUE((folly::less_than<uint8_t, 0, int8_t>(-1)));
  EXPECT_TRUE((folly::less_than<uint16_t, 0, int16_t>(-1)));
  EXPECT_TRUE((folly::less_than<uint32_t, 0, int32_t>(-1)));
  EXPECT_TRUE((folly::less_than<uint64_t, 0, int64_t>(-1)));

  EXPECT_FALSE((folly::less_than<int8_t, -1, uint8_t>(0)));
  EXPECT_FALSE((folly::less_than<int16_t, -1, uint16_t>(0)));
  EXPECT_FALSE((folly::less_than<int32_t, -1, uint32_t>(0)));
  EXPECT_FALSE((folly::less_than<int64_t, -1, uint64_t>(0)));

  EXPECT_FALSE((folly::greater_than<uint8_t, 0u, uint8_t>(0u)));
  EXPECT_TRUE((folly::greater_than<uint8_t, 0u, uint8_t>(254u)));
  EXPECT_FALSE((folly::greater_than<uint8_t, 255u, uint8_t>(255u)));
  EXPECT_FALSE((folly::greater_than<uint8_t, 255u, uint8_t>(254u)));

  EXPECT_FALSE((folly::greater_than<uint8_t, 0, int8_t>(-1)));
  EXPECT_FALSE((folly::greater_than<uint16_t, 0, int16_t>(-1)));
  EXPECT_FALSE((folly::greater_than<uint32_t, 0, int32_t>(-1)));
  EXPECT_FALSE((folly::greater_than<uint64_t, 0, int64_t>(-1)));

  EXPECT_TRUE((folly::greater_than<int8_t, -1, uint8_t>(0)));
  EXPECT_TRUE((folly::greater_than<int16_t, -1, uint16_t>(0)));
  EXPECT_TRUE((folly::greater_than<int32_t, -1, uint32_t>(0)));
  EXPECT_TRUE((folly::greater_than<int64_t, -1, uint64_t>(0)));
}

TEST(Traits, is_non_bool_integral) {
  EXPECT_TRUE((folly::is_non_bool_integral_v<int>));
  EXPECT_TRUE((folly::is_non_bool_integral_v<int const volatile>));
  EXPECT_FALSE((folly::is_non_bool_integral_v<bool>));
  EXPECT_FALSE((folly::is_non_bool_integral_v<bool const volatile>));
}

#if FOLLY_HAVE_INT128_T

TEST(Traits, int128) {
  EXPECT_TRUE(
      (::std::is_same<folly::make_unsigned_t<int128_t>, uint128_t>::value));
  EXPECT_TRUE(
      (::std::is_same<folly::make_signed_t<int128_t>, int128_t>::value));
  EXPECT_TRUE(
      (::std::is_same<folly::make_unsigned_t<uint128_t>, uint128_t>::value));
  EXPECT_TRUE( //
      (::std::is_same<folly::make_signed_t<uint128_t>, int128_t>::value));
  EXPECT_TRUE((folly::is_arithmetic_v<int128_t>));
  EXPECT_TRUE((folly::is_arithmetic_v<uint128_t>));
  EXPECT_TRUE((folly::is_integral_v<int128_t>));
  EXPECT_TRUE((folly::is_integral_v<uint128_t>));
  EXPECT_FALSE((folly::is_unsigned_v<int128_t>));
  EXPECT_TRUE((folly::is_signed_v<int128_t>));
  EXPECT_TRUE((folly::is_unsigned_v<uint128_t>));
  EXPECT_FALSE((folly::is_signed_v<__uint128_t>));
}

#endif // FOLLY_HAVE_INT128_T

template <typename T, typename... Args>
void testIsRelocatable(Args&&... args) {
  if (!IsRelocatable<T>::value) {
    return;
  }

  // We use placement new on zeroed memory to avoid garbage subsections
  char vsrc[sizeof(T)] = {0};
  char vdst[sizeof(T)] = {0};
  char vcpy[sizeof(T)];

  T* src = new (vsrc) T(std::forward<Args>(args)...);
  SCOPE_EXIT {
    src->~T();
  };
  std::memcpy(vcpy, vsrc, sizeof(T));
  T deep(*src);
  T* dst = new (vdst) T(std::move(*src));
  SCOPE_EXIT {
    dst->~T();
  };

  EXPECT_EQ(deep, *dst);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
  EXPECT_EQ(deep, *reinterpret_cast<T*>(vcpy));
#pragma GCC diagnostic pop

  // This test could technically fail; however, this is what relocation
  // almost always means, so it's a good test to have
  EXPECT_EQ(std::memcmp(vcpy, vdst, sizeof(T)), 0);
}

TEST(Traits, actuallyRelocatable) {
  // Ensure that we test stack and heap allocation for strings with in-situ
  // capacity
  testIsRelocatable<std::string>("1");
  testIsRelocatable<std::string>(sizeof(std::string) + 1, 'x');

  testIsRelocatable<std::vector<char>>(5, 'g');
}

struct inspects_tag {
  template <typename T>
  std::false_type is_char(tag_t<T>) const {
    return {};
  }
  std::true_type is_char(tag_t<char>) const { return {}; }
};

TEST(Traits, tag) {
  inspects_tag f;
  EXPECT_FALSE(f.is_char(tag_t<int>{}));
  EXPECT_TRUE(f.is_char(tag_t<char>{}));
  EXPECT_FALSE(f.is_char(tag<int>));
  EXPECT_TRUE(f.is_char(tag<char>));
}

namespace {
struct some_tag {};

template <typename T>
struct container {
  template <class... Args>
  explicit container(
      folly::type_t<some_tag, decltype(T(std::declval<Args>()...))>,
      Args&&...) {}
};
} // namespace

TEST(Traits, typeT) {
  EXPECT_TRUE((::std::is_same<folly::type_t<float>, float>::value));
  EXPECT_TRUE((::std::is_same<folly::type_t<float, int>, float>::value));
  EXPECT_TRUE((::std::is_same<folly::type_t<float, int, short>, float>::value));
  EXPECT_TRUE((
      ::std::is_same<folly::type_t<float, int, short, std::string>, float>::
          value));
  EXPECT_TRUE((
      ::std::is_constructible<::container<std::string>, some_tag, std::string>::
          value));
  EXPECT_FALSE((
      ::std::is_constructible<::container<std::string>, some_tag, float>::
          value));
}

namespace {
template <typename T, typename V>
using detector_find = decltype(std::declval<T>().find(std::declval<V>()));
}

TEST(Traits, detectedOrT) {
  EXPECT_TRUE(( //
      std::is_same<
          folly::detected_or_t<float, detector_find, std::string, char>,
          std::string::size_type>::value));
  EXPECT_TRUE(( //
      std::is_same<
          folly::detected_or_t<float, detector_find, double, char>,
          float>::value));
}

TEST(Traits, detectedT) {
  EXPECT_TRUE(( //
      std::is_same<
          folly::detected_t<detector_find, std::string, char>,
          std::string::size_type>::value));
  EXPECT_TRUE(( //
      std::is_same<
          folly::detected_t<detector_find, double, char>,
          folly::nonesuch>::value));
}

TEST(Traits, isDetected) {
  EXPECT_TRUE((folly::is_detected<detector_find, std::string, char>::value));
  EXPECT_FALSE((folly::is_detected<detector_find, double, char>::value));
}

TEST(Traits, isDetectedV) {
  EXPECT_TRUE((folly::is_detected_v<detector_find, std::string, char>));
  EXPECT_FALSE((folly::is_detected_v<detector_find, double, char>));
}

TEST(Traits, alignedStorageForT) {
  struct alignas(2) Foo {
    char data[4];
  };
  using storage = aligned_storage_for_t<Foo[4]>;
  EXPECT_EQ(16, sizeof(storage));
  EXPECT_EQ(2, alignof(storage));
  EXPECT_TRUE(std::is_trivial<storage>::value);
  EXPECT_TRUE(std::is_standard_layout<storage>::value);
}

TEST(Traits, alignedStorageTDefaultAlign) {
  // Default alignment matches std::aligned_storage: the most stringent
  // fundamental alignment for any type whose size is no greater than Len.
  // On all platforms, fundamental alignments are powers of 2 up to
  // alignof(max_align_t).
  constexpr auto kMaxAlign = alignof(std::max_align_t);

  // Helper: the expected default alignment for a given Len is the largest
  // power of 2 that is <= min(Len, alignof(max_align_t)).
  constexpr auto expected = [](std::size_t len) {
    std::size_t a = kMaxAlign;
    while (a > len) {
      a >>= 1;
    }
    return a ? a : 1;
  };

  // Small sizes: default alignment is the largest power of 2 <= Len.
  EXPECT_EQ(expected(1), alignof(folly::aligned_storage_t<1>));
  EXPECT_EQ(expected(2), alignof(folly::aligned_storage_t<2>));
  EXPECT_EQ(expected(3), alignof(folly::aligned_storage_t<3>));
  EXPECT_EQ(expected(4), alignof(folly::aligned_storage_t<4>));
  EXPECT_EQ(expected(5), alignof(folly::aligned_storage_t<5>));
  EXPECT_EQ(expected(6), alignof(folly::aligned_storage_t<6>));
  EXPECT_EQ(expected(7), alignof(folly::aligned_storage_t<7>));
  EXPECT_EQ(expected(8), alignof(folly::aligned_storage_t<8>));
  EXPECT_EQ(expected(9), alignof(folly::aligned_storage_t<9>));
  EXPECT_EQ(expected(15), alignof(folly::aligned_storage_t<15>));
  EXPECT_EQ(expected(16), alignof(folly::aligned_storage_t<16>));

  // Sizes beyond alignof(max_align_t): clamped to alignof(max_align_t).
  EXPECT_EQ(kMaxAlign, alignof(folly::aligned_storage_t<17>));
  EXPECT_EQ(kMaxAlign, alignof(folly::aligned_storage_t<32>));
  EXPECT_EQ(kMaxAlign, alignof(folly::aligned_storage_t<64>));
  EXPECT_EQ(kMaxAlign, alignof(folly::aligned_storage_t<256>));
  EXPECT_EQ(kMaxAlign, alignof(folly::aligned_storage_t<4096>));

  // Size is at least Len, rounded up to default alignment.
  EXPECT_EQ(1, sizeof(folly::aligned_storage_t<1>));
  EXPECT_EQ(48, sizeof(folly::aligned_storage_t<48>));
  EXPECT_GE(sizeof(folly::aligned_storage_t<3>), 3);
  EXPECT_GE(sizeof(folly::aligned_storage_t<5>), 5);
  EXPECT_EQ(0, sizeof(folly::aligned_storage_t<3>) % expected(3));
  EXPECT_EQ(0, sizeof(folly::aligned_storage_t<5>) % expected(5));

  // Explicit alignment overrides the default.
  EXPECT_EQ(1, alignof(folly::aligned_storage_t<64, 1>));
  EXPECT_EQ(64, alignof(folly::aligned_storage_t<1, 64>));

  // Type properties.
  EXPECT_TRUE(std::is_trivial<folly::aligned_storage_t<16>>::value);
  EXPECT_TRUE(std::is_standard_layout<folly::aligned_storage_t<16>>::value);
}

TEST(Traits, removeCvref) {
  using folly::remove_cvref;
  using folly::remove_cvref_t;

  // test all possible c-ref qualifiers without volatile
  EXPECT_TRUE((std::is_same<remove_cvref_t<int>, int>::value));
  EXPECT_TRUE((std::is_same<remove_cvref<int>::type, int>::value));

  EXPECT_TRUE((std::is_same<remove_cvref_t<int&&>, int>::value));
  EXPECT_TRUE((std::is_same<remove_cvref<int&&>::type, int>::value));

  EXPECT_TRUE((std::is_same<remove_cvref_t<int&>, int>::value));
  EXPECT_TRUE((std::is_same<remove_cvref<int&>::type, int>::value));

  EXPECT_TRUE((std::is_same<remove_cvref_t<int const>, int>::value));
  EXPECT_TRUE((std::is_same<remove_cvref<int const>::type, int>::value));

  EXPECT_TRUE((std::is_same<remove_cvref_t<int const&>, int>::value));
  EXPECT_TRUE((std::is_same<remove_cvref<int const&>::type, int>::value));

  EXPECT_TRUE((std::is_same<remove_cvref_t<int const&&>, int>::value));
  EXPECT_TRUE((std::is_same<remove_cvref<int const&&>::type, int>::value));

  // test all possible c-ref qualifiers with volatile
  EXPECT_TRUE((std::is_same<remove_cvref_t<int volatile>, int>::value));
  EXPECT_TRUE((std::is_same<remove_cvref<int volatile>::type, int>::value));

  EXPECT_TRUE((std::is_same<remove_cvref_t<int volatile&&>, int>::value));
  EXPECT_TRUE((std::is_same<remove_cvref<int volatile&&>::type, int>::value));

  EXPECT_TRUE((std::is_same<remove_cvref_t<int volatile&>, int>::value));
  EXPECT_TRUE((std::is_same<remove_cvref<int volatile&>::type, int>::value));

  EXPECT_TRUE((std::is_same<remove_cvref_t<int volatile const>, int>::value));
  EXPECT_TRUE(
      (std::is_same<remove_cvref<int volatile const>::type, int>::value));

  EXPECT_TRUE((std::is_same<remove_cvref_t<int volatile const&>, int>::value));
  EXPECT_TRUE(
      (std::is_same<remove_cvref<int volatile const&>::type, int>::value));

  EXPECT_TRUE((std::is_same<remove_cvref_t<int volatile const&&>, int>::value));
  EXPECT_TRUE(
      (std::is_same<remove_cvref<int volatile const&&>::type, int>::value));
}

TEST(Traits, copy_cvref) {
  static_assert(std::is_same_v<copy_cvref_t<int, char>, char>);
  static_assert(std::is_same_v<copy_cvref_t<int const, char>, char const>);
  static_assert(
      std::is_same_v<copy_cvref_t<int volatile, char>, char volatile>);
  static_assert( //
      std::is_same_v<
          copy_cvref_t<int const volatile, char>,
          char const volatile>);
  static_assert(std::is_same_v<copy_cvref_t<int&, char>, char&>);
  static_assert(std::is_same_v<copy_cvref_t<int const&, char>, char const&>);
  static_assert(
      std::is_same_v<copy_cvref_t<int volatile&, char>, char volatile&>);
  static_assert( //
      std::is_same_v<
          copy_cvref_t<int const volatile&, char>,
          char const volatile&>);
  static_assert(std::is_same_v<copy_cvref_t<int&&, char>, char&&>);
  static_assert(std::is_same_v<copy_cvref_t<int const&&, char>, char const&&>);
  static_assert(
      std::is_same_v<copy_cvref_t<int volatile&&, char>, char volatile&&>);
  static_assert( //
      std::is_same_v<
          copy_cvref_t<int const volatile&&, char>,
          char const volatile&&>);
}

TEST(Traits, like) {
  static_assert(std::is_same_v<like_t<int, char>, char>);
  static_assert(std::is_same_v<like_t<int const, char>, char const>);
  static_assert(std::is_same_v<like_t<int volatile, char>, char>);
  static_assert(
      std::is_same_v<like_t<int const, char volatile>, char const volatile>);
  static_assert(std::is_same_v<like_t<int&, char>, char&>);
  static_assert(std::is_same_v<like_t<int const&, char>, char const&>);
  static_assert(std::is_same_v<like_t<int volatile&, char>, char&>);
  static_assert(
      std::is_same_v<like_t<int const&, char volatile>, char const volatile&>);
  static_assert(std::is_same_v<like_t<int&&, char>, char&&>);
  static_assert(std::is_same_v<like_t<int const&&, char>, char const&&>);
  static_assert(std::is_same_v<like_t<int volatile&&, char>, char&&>);
  static_assert( //
      std::is_same_v< //
          like_t<int const&&, char volatile>,
          char const volatile&&>);
  // Check we don't strip underlying `const` from `Dst`
  static_assert(std::is_same_v<like_t<int, const char>, const char>);
  static_assert(std::is_same_v<like_t<int, const char&>, const char>);
}

TEST(Traits, UncvrefSameAs) {
  static_assert(folly::uncvref_same_as<std::vector<int>, std::vector<int>>);
  static_assert(folly::uncvref_same_as<std::vector<int>&, std::vector<int>>);
  static_assert(
      folly::uncvref_same_as<const std::vector<int>&, std::vector<int>>);
  static_assert(folly::uncvref_same_as<std::vector<int>&&, std::vector<int>>);

  constexpr auto refersToExample =
      [](folly::uncvref_same_as<std::vector<int>> auto&&) {};

  static_assert(std::invocable<decltype(refersToExample), std::vector<int>>);
  static_assert(
      std::invocable<decltype(refersToExample), const std::vector<int>&>);
  static_assert(std::invocable<decltype(refersToExample), std::vector<int>&&>);

  static_assert(
      !std::invocable<decltype(refersToExample), std::vector<char>&&>);
}

TEST(Traits, isInstantiationOfV) {
  EXPECT_TRUE((is_instantiation_of_v<A, A<int>>));
  EXPECT_FALSE((is_instantiation_of_v<A, B>));
}

TEST(Traits, isInstantiationOf) {
  EXPECT_TRUE((is_instantiation_of<A, A<int>>::value));
  EXPECT_FALSE((is_instantiation_of<A, B>::value));
}

TEST(Traits, InstantiationOf) {
  static_assert(folly::instantiated_from<A<int>, A>);
  static_assert(!folly::instantiated_from<A<int>&, A>);
  static_assert(!folly::instantiated_from<A<int>, std::vector>);

  static_assert(folly::uncvref_instantiated_from<A<int>, A>);
  static_assert(folly::uncvref_instantiated_from<A<int>&, A>);
  static_assert(!folly::uncvref_instantiated_from<A<int>&, std::vector>);

  auto example = [](folly::uncvref_instantiated_from<std::vector> auto&&) {};

  static_assert(std::invocable<decltype(example), std::vector<int>&&>);
  static_assert(std::invocable<decltype(example), std::vector<int>&>);
  static_assert(std::invocable<decltype(example), const std::vector<int>&>);
  static_assert(std::invocable<decltype(example), std::vector<int>>);
}

TEST(Traits, member_pointer_traits_data) {
  struct o {};
  using d = float;
  using mp_do = float o::*;
  using mp_traits_do = folly::member_pointer_traits<mp_do>;
  EXPECT_TRUE((std::is_same<d, mp_traits_do::member_type>::value));
  EXPECT_TRUE((std::is_same<o, mp_traits_do::object_type>::value));
}

TEST(Traits, member_pointer_traits_function) {
  struct o {};
  using f = float(char*) const;
  using mp_fo = float (o::*)(char*) const;
  using mp_traits_fo = folly::member_pointer_traits<mp_fo>;
  EXPECT_TRUE((std::is_same<f, mp_traits_fo::member_type>::value));
  EXPECT_TRUE((std::is_same<o, mp_traits_fo::object_type>::value));
}

TEST(Traits, isConstexprDefaultConstructible) {
  EXPECT_TRUE(is_constexpr_default_constructible_v<int>);
  EXPECT_TRUE(is_constexpr_default_constructible<int>{});

  struct Empty {};
  EXPECT_TRUE(is_constexpr_default_constructible_v<Empty>);
  EXPECT_TRUE(is_constexpr_default_constructible<Empty>{});

  struct NonTrivialDtor {
    [[maybe_unused]] ~NonTrivialDtor() {}
  };
  EXPECT_FALSE(is_constexpr_default_constructible_v<NonTrivialDtor>);
  EXPECT_FALSE(is_constexpr_default_constructible<NonTrivialDtor>{});

  struct ConstexprCtor {
    int x, y;
    constexpr ConstexprCtor() noexcept : x(7), y(11) {}
  };
  EXPECT_TRUE(is_constexpr_default_constructible_v<ConstexprCtor>);
  EXPECT_TRUE(is_constexpr_default_constructible<ConstexprCtor>{});

  struct NonConstexprCtor {
    int x, y;
    NonConstexprCtor() noexcept : x(7), y(11) {}
  };
  EXPECT_FALSE(is_constexpr_default_constructible_v<NonConstexprCtor>);
  EXPECT_FALSE(is_constexpr_default_constructible<NonConstexprCtor>{});

  struct NoDefaultCtor {
    constexpr NoDefaultCtor(int, int) noexcept {}
  };
  EXPECT_FALSE(is_constexpr_default_constructible_v<NoDefaultCtor>);
  EXPECT_FALSE(is_constexpr_default_constructible<NoDefaultCtor>{});
}

TEST(Traits, uintBits) {
  EXPECT_TRUE((std::is_same_v<uint8_t, uint_bits_t<8>>));
  EXPECT_TRUE((std::is_same_v<uint16_t, uint_bits_t<16>>));
  EXPECT_TRUE((std::is_same_v<uint32_t, uint_bits_t<32>>));
  EXPECT_TRUE((std::is_same_v<uint64_t, uint_bits_t<64>>));
#if FOLLY_HAVE_INT128_T
  EXPECT_TRUE((std::is_same_v<uint128_t, uint_bits_t<128>>));
#endif // FOLLY_HAVE_INT128_T
}

TEST(Traits, uintBitsLg) {
  EXPECT_TRUE((std::is_same_v<uint8_t, uint_bits_lg_t<3>>));
  EXPECT_TRUE((std::is_same_v<uint16_t, uint_bits_lg_t<4>>));
  EXPECT_TRUE((std::is_same_v<uint32_t, uint_bits_lg_t<5>>));
  EXPECT_TRUE((std::is_same_v<uint64_t, uint_bits_lg_t<6>>));
#if FOLLY_HAVE_INT128_T
  EXPECT_TRUE((std::is_same_v<uint128_t, uint_bits_lg_t<7>>));
#endif // FOLLY_HAVE_INT128_T
}

TEST(Traits, intBits) {
  EXPECT_TRUE((std::is_same_v<int8_t, int_bits_t<8>>));
  EXPECT_TRUE((std::is_same_v<int16_t, int_bits_t<16>>));
  EXPECT_TRUE((std::is_same_v<int32_t, int_bits_t<32>>));
  EXPECT_TRUE((std::is_same_v<int64_t, int_bits_t<64>>));
#if FOLLY_HAVE_INT128_T
  EXPECT_TRUE((std::is_same_v<int128_t, int_bits_t<128>>));
#endif // FOLLY_HAVE_INT128_T
}

TEST(Traits, intBitsLg) {
  EXPECT_TRUE((std::is_same_v<int8_t, int_bits_lg_t<3>>));
  EXPECT_TRUE((std::is_same_v<int16_t, int_bits_lg_t<4>>));
  EXPECT_TRUE((std::is_same_v<int32_t, int_bits_lg_t<5>>));
  EXPECT_TRUE((std::is_same_v<int64_t, int_bits_lg_t<6>>));
#if FOLLY_HAVE_INT128_T
  EXPECT_TRUE((std::is_same_v<int128_t, int_bits_lg_t<7>>));
#endif // FOLLY_HAVE_INT128_T
}

TEST(Traits, isAllocator) {
  static_assert(is_allocator_v<std::allocator<int>>);
  static_assert(is_allocator<std::allocator<int>>::value);

  static_assert(is_allocator_v<std::allocator<std::string>>);
  static_assert(is_allocator<std::allocator<std::string>>::value);

  static_assert(!is_allocator_v<int>);
  static_assert(!is_allocator<int>::value);

  static_assert(!is_allocator_v<std::string>);
  static_assert(!is_allocator<std::string>::value);
}

struct type_pack_element_test {
  template <size_t I, typename... T>
  using fallback = traits_detail::type_pack_element_fallback<I, T...>;
  template <size_t I, typename... T>
  using native = type_pack_element_t<I, T...>;

  template <typename IC, typename... T>
  using fallback_ic = fallback<IC::value, T...>;
  template <typename IC, typename... T>
  using native_ic = native<IC::value, T...>;
};

TEST(Traits, type_pack_element) {
  using test = type_pack_element_test;

  EXPECT_TRUE(( //
      std::is_same_v<
          test::fallback<3, int, int, int, double, int, int>,
          double>));
  EXPECT_TRUE((std::is_same_v<test::fallback<0, int[1]>, int[1]>));
  EXPECT_TRUE((is_detected_v<test::fallback_ic, index_constant<0>, int>));
  EXPECT_FALSE((is_detected_v<test::fallback_ic, index_constant<0>>));

  EXPECT_TRUE(( //
      std::is_same_v<
          test::native<3, int, int, int, double, int, int>, //
          double>));
  EXPECT_TRUE((std::is_same_v<test::native<0, int[1]>, int[1]>));
  EXPECT_TRUE((is_detected_v<test::native_ic, index_constant<0>, int>));
  EXPECT_FALSE((is_detected_v<test::native_ic, index_constant<0>>));
}

TEST(Traits, type_pack_size) {
  EXPECT_EQ(0, (type_pack_size_v<>));
  EXPECT_EQ(1, (type_pack_size_v<int>));
  EXPECT_EQ(5, (type_pack_size_v<long long, long, int, short, char>));

  EXPECT_EQ(0, (type_pack_size_t<>::value));
  EXPECT_EQ(1, (type_pack_size_t<int>::value));
  EXPECT_EQ(5, (type_pack_size_t<long long, long, int, short, char>::value));
}

TEST(Traits, type_list_element) {
  EXPECT_TRUE(( //
      std::is_same_v<
          folly::type_list_element_t<
              3,
              folly::tag_t<int, int, int, double, int, int>>, //
          double>));
  EXPECT_TRUE(( //
      std::is_same_v<
          folly::type_list_element_t<0, folly::tag_t<int[1]>>,
          int[1]>));
}

TEST(Traits, type_list_size) {
  EXPECT_EQ(0, (type_list_size_v<tag_t<>>));
  EXPECT_EQ(1, (type_list_size_v<tag_t<int>>));
  EXPECT_EQ(5, (type_list_size_v<tag_t<long long, long, int, short, char>>));

  EXPECT_EQ(0, (type_list_size_t<tag_t<>>::value));
  EXPECT_EQ(1, (type_list_size_t<tag_t<int>>::value));
  EXPECT_EQ(
      5, (type_list_size_t<tag_t<long long, long, int, short, char>>::value));
}

TEST(Traits, type_list_concat) {
  static_assert(std::is_same_v<tag_t<>, type_list_concat_t<tag_t>>);
  static_assert(std::is_same_v<tag_t<>, type_list_concat_t<tag_t, tag_t<>>>);
  static_assert(
      std::is_same_v<tag_t<>, type_list_concat_t<tag_t, tag_t<>, tag_t<>>>);
  static_assert(
      std::is_same_v<tag_t<int>, type_list_concat_t<tag_t, tag_t<int>>>);
  static_assert( //
      std::is_same_v<
          tag_t<int, double>,
          type_list_concat_t<tag_t, tag_t<int>, tag_t<double>>>);
  static_assert( //
      std::is_same_v<
          tag_t<int, void, double, inspects_tag>,
          type_list_concat_t<
              tag_t,
              std::tuple<int, void, double>,
              tag_t<>,
              tag_t<inspects_tag>>>);
}

TEST(Traits, value_pack) {
  EXPECT_EQ(3, (folly::value_pack_size_v<7u, 8, '9'>));
  EXPECT_EQ(3, (folly::value_pack_size_t<7u, 8, '9'>::value));
  EXPECT_TRUE(( //
      std::is_same_v<int, folly::value_pack_element_type_t<1, 7u, 8, '9'>>));
  EXPECT_EQ(8, (folly::value_pack_element_v<1, 7u, 8, '9'>));
}

TEST(Traits, value_list) {
  EXPECT_EQ(3, (folly::value_list_size_v<vtag_t<7u, 8, '9'>>));
  EXPECT_EQ(3, (folly::value_list_size_t<vtag_t<7u, 8, '9'>>::value));
  EXPECT_TRUE(( //
      std::is_same_v<
          int,
          folly::value_list_element_type_t<1, vtag_t<7u, 8, '9'>>>));
  EXPECT_EQ(8, (folly::value_list_element_v<1, vtag_t<7u, 8, '9'>>));
}

template <auto...>
struct also_vtag_t {};

TEST(Traits, value_list_concat) {
  static_assert(std::is_same_v<vtag_t<>, value_list_concat_t<vtag_t>>);
  static_assert(
      std::is_same_v<vtag_t<>, value_list_concat_t<vtag_t, vtag_t<>>>);
  static_assert( //
      std::is_same_v< //
          vtag_t<>,
          value_list_concat_t<vtag_t, vtag_t<>, vtag_t<>>>);
  static_assert(
      std::is_same_v<vtag_t<7>, value_list_concat_t<vtag_t, vtag_t<7>>>);
  static_assert( //
      std::is_same_v<
          vtag_t<3, 1, 4, 1, 5, 9>,
          value_list_concat_t<
              vtag_t,
              also_vtag_t<3, 1>,
              vtag_t<>,
              vtag_t<4>,
              vtag_t<1, 5, 9>>>);
}

TEST(Traits, type_pack_find) {
  EXPECT_EQ(0, folly::type_pack_find_v<int>);
  EXPECT_EQ(0, folly::type_pack_find_t<int>{});
  EXPECT_EQ(4, (folly::type_pack_find_v<long, char, short, int, float>));
  EXPECT_EQ(4, (folly::type_pack_find_t<long, char, short, int, float>{}));
  EXPECT_EQ(2, (folly::type_pack_find_v<int, char, short, int, float>));
  EXPECT_EQ(2, (folly::type_pack_find_t<int, char, short, int, float>{}));
}

TEST(Traits, type_list_find) {
  EXPECT_EQ(0, (folly::type_list_find_v<int, folly::tag_t<>>));
  EXPECT_EQ(0, (folly::type_list_find_t<int, folly::tag_t<>>{}));
  EXPECT_EQ(
      4,
      (folly::type_list_find_v<long, folly::tag_t<char, short, int, float>>));
  EXPECT_EQ(
      4,
      (folly::type_list_find_t<long, folly::tag_t<char, short, int, float>>{}));
  EXPECT_EQ(
      2, (folly::type_list_find_v<int, folly::tag_t<char, short, int, float>>));
  EXPECT_EQ(
      2,
      (folly::type_list_find_t<int, folly::tag_t<char, short, int, float>>{}));
}
