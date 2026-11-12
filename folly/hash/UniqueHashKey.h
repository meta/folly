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

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

#include <folly/Traits.h>
#include <folly/portability/Config.h>

// Detect the headers here rather than in folly-config.h: in modular builds,
// folly-config.h is compiled without this library's deps on the include path.
#ifndef FOLLY_HAVE_BLAKE3
#if __has_include(<blake3.h>)
#define FOLLY_HAVE_BLAKE3 1
#else
#define FOLLY_HAVE_BLAKE3 0
#endif
#endif

#ifndef FOLLY_HAVE_XXH3
#if __has_include(<xxh3.h>)
#define FOLLY_HAVE_XXH3 1
#else
#define FOLLY_HAVE_XXH3 0
#endif
#endif

namespace folly {

namespace detail {

struct unique_hash_key_item {
  static inline constexpr size_t len_include_mask = ~(~size_t(0) >> 1);

  uint8_t const* buf{};
  size_t len{}; // hi bit set means include len in hash

  std::pair<size_t, bool> unpack_len() const {
    constexpr auto mask = len_include_mask;
    return {len & ~mask, len & mask};
  }
};

} // namespace detail

/// unique_hash_key_algo_strong_sha256_fn
///
/// Hashes std::span<detail::unique_hash_key_item const> using SHA256 with a key
/// that may vary between processes.
///
/// The size must be in {8, 16, 24, 32}.
///
/// For use with unique_hash_key.
template <size_t Size>
struct unique_hash_key_algo_strong_sha256_fn {
  static_assert(Size <= 32);
  static_assert(Size % 8 == 0);
  std::array<uint8_t, Size> operator()(
      std::span<detail::unique_hash_key_item const> in) const noexcept;
};
template <size_t Size>
inline constexpr unique_hash_key_algo_strong_sha256_fn<Size>
    unique_hash_key_algo_strong_sha256{};

#if FOLLY_HAVE_BLAKE3

/// unique_hash_key_algo_strong_blake3_fn
///
/// Hashes std::span<detail::unique_hash_key_item const> using BLAKE3 with a key
/// that may vary between processes.
///
/// The size must be in {8, 16, 24, 32}.
///
/// For use with unique_hash_key.
template <size_t Size>
struct unique_hash_key_algo_strong_blake3_fn {
  static_assert(Size <= 32);
  static_assert(Size % 8 == 0);
  std::array<uint8_t, Size> operator()(
      std::span<detail::unique_hash_key_item const> in) const noexcept;
};
template <size_t Size>
inline constexpr unique_hash_key_algo_strong_blake3_fn<Size>
    unique_hash_key_algo_strong_blake3{};

#if FOLLY_HAVE_XXH3

/// unique_hash_key_algo_fast_xxh3_fn
///
/// Hashes std::span<detail::unique_hash_key_item const> using XXH3 with a key
/// that may vary between processes.
///
/// The size must be in {8, 16}, corresponding to XXH3_64bits and XXH3_128bits.
///
/// For use with unique_hash_key.
template <size_t Size>
struct unique_hash_key_algo_fast_xxh3_fn {
  static_assert(Size <= 16);
  static_assert(Size % 8 == 0);
  std::array<uint8_t, Size> operator()(
      std::span<detail::unique_hash_key_item const> in) const noexcept;
};
template <size_t Size>
inline constexpr unique_hash_key_algo_fast_xxh3_fn<Size>
    unique_hash_key_algo_fast_xxh3{};

#endif // FOLLY_HAVE_XXH3

#endif // FOLLY_HAVE_BLAKE3

template <auto Algo>
constexpr size_t unique_hash_key_algo_size_v =
    decltype(Algo(std::span<detail::unique_hash_key_item const>{})){}.size();

/// unique_hash_key
///
/// A unique hashtable key cryptographically hashed from input data.
///
/// The size must be in {8, 16, 24, 32}.
///
/// Uniqueness is only guaranteed between two constructions when all of these
/// conditions hold:
/// * At sufficiently large sizes, likely at least size 16.
/// * For cryptographically strong hash algorithms, such as BLAKE3.
/// * With equivalent hash algorithm objects of the same type.
/// * With packs of hashable arguments which have the same sequences of types.
/// * With packs of hashable arguments that differ in at least one element.
template <size_t Size>
class unique_hash_key {
 private:
  using self = unique_hash_key;

  static inline constexpr size_t data_size = Size;
  static inline constexpr size_t data_align = alignof(size_t);

  static_assert(data_size <= 32);
  static_assert(data_size % 8 == 0);

  using item_type = detail::unique_hash_key_item;
  using data_type = std::array<uint8_t, Size>;

  template <typename Algo>
  static inline constexpr bool is_algo_v =
      std::is_invocable_v<Algo const&, std::span<item_type const>>;

  alignas(data_align) data_type const data_;

  template <
      typename Int,
      std::enable_if_t<is_non_bool_integral_v<Int>, int> = 0>
  static item_type init_item(
      Int const& arg,
      [[maybe_unused]] size_t idx,
      [[maybe_unused]] size_t size) {
    auto const buf = reinterpret_cast<uint8_t const*>(&arg);
    return {buf, sizeof(arg)};
  }
  static item_type init_item(std::string_view arg, size_t idx, size_t size) {
    constexpr auto mask = item_type::len_include_mask;
    auto const buf = reinterpret_cast<uint8_t const*>(arg.data());
    auto const hi = mask * size_t(idx + 1 < size);
    return {buf, arg.size() | hi};
  }

  template <typename Algo, typename... Arg>
  static data_type init(Algo const algo, Arg const&... arg) noexcept {
    constexpr auto len = sizeof...(Arg);
    size_t idxr = 0;
    size_t idxl = 0;
    std::array<item_type, len> items{};
    ((items[idxl++] = init_item(arg, idxr++, len)), ...);
    return algo(items);
  }

  template <typename T, size_t E, typename V = std::remove_cv_t<T>>
  static constexpr bool is_span_compatible_v = //
      !std::is_volatile_v<T> && //
      std::is_integral_v<V> && //
      std::is_unsigned_v<V> && //
      !std::is_same_v<bool, V> && //
      !std::is_same_v<char, V> && //
      alignof(V) <= data_align && //
      (E == data_size / sizeof(T) || E == std::dynamic_extent);

 public:
  template <
      typename Algo,
      typename... Arg,
      std::enable_if_t<is_algo_v<Algo>, int> = 0>
  explicit unique_hash_key(Algo const algo, Arg const&... arg) noexcept
      : data_{init(algo, arg...)} {
    static_assert(sizeof(self) == Size);
    static_assert(alignof(self) == alignof(size_t));
  }

  template <
      typename T,
      std::size_t E,
      std::enable_if_t<is_span_compatible_v<T, E>, int> = 0>
  explicit operator std::span<T const, E>() const noexcept {
    constexpr auto count = E == std::dynamic_extent ? data_size / sizeof(T) : E;
    return std::span<T const, E>{
        reinterpret_cast<T const*>(data_.data()), count};
  }

  friend auto operator==(self const& a, self const& b) noexcept {
    return a.data_ == b.data_;
  }
};

/// unique_hash_key_with
///
/// A unique-hash-key of the size returned by the given hash algorithm, using
/// the given algorithm.
///
/// Hash values are unique to the process. But see the caveats regarding
/// uniqueness above, with tuples replacing packs.
///
/// It is explicitly permitted to copy-construct unique_hash_key (base class)
/// instances from instances of unique_hash_key_with<Algo> (derived class).
/// There is no concern about object slicing.
template <auto Algo>
class unique_hash_key_with
    : public unique_hash_key<unique_hash_key_algo_size_v<Algo>> {
 private:
  static constexpr size_t size = unique_hash_key_algo_size_v<Algo>;
  using self = unique_hash_key_with;
  using base = unique_hash_key<size>;

  template <typename... Arg, size_t... Idx>
  explicit unique_hash_key_with(
      std::tuple<Arg...> const& arg, std::index_sequence<Idx...>)
      : base(Algo, std::get<Idx>(arg)...) {}

 public:
  template <typename... Arg>
  explicit unique_hash_key_with(std::tuple<Arg...> const& arg)
      : self(arg, std::index_sequence_for<Arg...>{}) {}
};

/// unique_hash_key_strong_sha256
///
/// A unique-hash-key of the given size using the cryptographically strong
/// SHA256 hash algorithm from OpenSSL.
///
/// Hash values are unique to the process. But see the caveats regarding
/// uniqueness above, with tuples replacing packs.
template <size_t Size>
using unique_hash_key_strong_sha256 =
    unique_hash_key_with<unique_hash_key_algo_strong_sha256<Size>>;

#if FOLLY_HAVE_BLAKE3

/// unique_hash_key_strong_blake3
///
/// A unique-hash-key of the given size using the cryptographically strong
/// BLAKE3 hash algorithm.
///
/// Hash values are unique to the process. But see the caveats regarding
/// uniqueness above, with tuples replacing packs.
template <size_t Size>
using unique_hash_key_strong_blake3 =
    unique_hash_key_with<unique_hash_key_algo_strong_blake3<Size>>;

#if FOLLY_HAVE_XXH3

/// unique_hash_key_strong_blake3
///
/// A unique-hash-key of the given size using the fast but not cryptographically
/// strong XXH3 algorithm.
///
/// Hash values are unique to the process. But see the caveats regarding
/// uniqueness above, with tuples replacing packs.
template <size_t Size>
using unique_hash_key_fast_xxh3 =
    unique_hash_key_with<unique_hash_key_algo_fast_xxh3<Size>>;

#endif // FOLLY_HAVE_XXH3

#endif // FOLLY_HAVE_BLAKE3

} // namespace folly

namespace std {

template <size_t Size>
struct hash<::folly::unique_hash_key<Size>> {
  using folly_is_avalanching = std::true_type;

  size_t operator()(::folly::unique_hash_key<Size> const& key) const noexcept {
    return ::std::span<size_t const>{key}[0];
  }
};

template <auto Algo>
struct hash<::folly::unique_hash_key_with<Algo>>
    : hash<::folly::unique_hash_key<
          ::folly::unique_hash_key_algo_size_v<Algo>>> {};

} // namespace std
