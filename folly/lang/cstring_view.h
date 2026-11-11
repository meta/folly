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

#include <cassert>
#include <cstring>
#include <functional>
#include <iosfwd>
#include <string_view>

#include <fmt/format.h>

#include <folly/Portability.h>
#include <folly/Traits.h>
#include <folly/Utility.h>

namespace folly {

/// cstring_view
///
/// A string view type that privately inherits from std::string_view but
/// guarantees that the underlying buffer is null-terminated. This allows safe
/// use of .c_str() while maintaining all the benefits of string_view.
///
/// The type provides adjusted constructors and assignment operators to ensure
/// the null-termination guarantee is maintained.
///
/// mimic: std::cstring_view, p3655r3
///
/// https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3655r3.html
template <typename Char, typename Traits = std::char_traits<Char>>
class basic_cstring_view {
  using self = basic_cstring_view;
  using view_type = std::basic_string_view<Char, Traits>;

  template <typename T>
  using detect_c_str = decltype(FOLLY_DECLVAL(T const&).c_str());

  static constexpr Char const* check_string(
      Char const* str, [[maybe_unused]] size_t len) noexcept {
    assert((!str && !len) || Traits::length(str) == len);
    return str;
  }
  template <typename String>
  static constexpr String const& check_string(String const& str) noexcept {
    check_string(str.c_str(), str.size());
    return str;
  }

 public:
  using const_iterator = typename view_type::const_iterator;
  using const_pointer = typename view_type::const_pointer;
  using const_reference = typename view_type::const_reference;
  using const_reverse_iterator = typename view_type::const_reverse_iterator;
  using difference_type = typename view_type::difference_type;
  using iterator = typename view_type::iterator;
  using pointer = typename view_type::pointer;
  using reference = typename view_type::reference;
  using reverse_iterator = typename view_type::reverse_iterator;
  using size_type = typename view_type::size_type;
  using traits_type = typename view_type::traits_type;
  using value_type = typename view_type::value_type;

  static constexpr size_type npos = view_type::npos;

 private:
  view_type view_;

 public:
  constexpr basic_cstring_view() noexcept = default;

  constexpr basic_cstring_view(std::nullptr_t) = delete;

  /* implicit */ constexpr basic_cstring_view(Char const* str) noexcept
      : view_(!str ? view_type{} : view_type{str}) {}

  /// Diverges from p3655r3 in that we allow construction from a null pointer.
  /// Members data() and c_str() will return nullptr. Mimics std::string_view
  /// behavior v.s. std::string behavior.
  ///
  /// The paper tries to make std::cstring_view work like a string and not like
  /// a view (a view is a structure with a non-owning pointer and a length),
  /// creating a discrepancy between std::string_view and std::cstring_view.
  constexpr basic_cstring_view(Char const* str, std::size_t len) noexcept
      : view_(check_string(str, len), len) {}

  template <typename String, typename = detect_c_str<String>>
  /* implicit */ constexpr basic_cstring_view(String const& str) noexcept
      : view_(check_string(str)) {}

  constexpr basic_cstring_view(basic_cstring_view const&) noexcept = default;

  constexpr basic_cstring_view& operator=(basic_cstring_view const&) noexcept =
      default;

  constexpr basic_cstring_view& operator=(Char const* str) noexcept {
    view_ = str;
    return *this;
  }

  template <typename String, typename..., typename = detect_c_str<String>>
  constexpr basic_cstring_view& operator=(String const& str) noexcept {
    view_ = check_string(str);
    return *this;
  }

  // Accessor methods
  constexpr const_reference operator[](size_type pos) const {
    return view_[pos];
  }
  constexpr const_reference at(size_type pos) const { return view_.at(pos); }
  constexpr const_reference front() const { return view_.front(); }
  constexpr const_reference back() const { return view_.back(); }
  constexpr const_pointer data() const noexcept { return view_.data(); }

  // Iterator methods
  constexpr const_iterator begin() const noexcept { return view_.begin(); }
  constexpr const_iterator end() const noexcept { return view_.end(); }
  constexpr const_iterator cbegin() const noexcept { return view_.cbegin(); }
  constexpr const_iterator cend() const noexcept { return view_.cend(); }
  constexpr const_reverse_iterator rbegin() const noexcept {
    return view_.rbegin();
  }
  constexpr const_reverse_iterator rend() const noexcept {
    return view_.rend();
  }
  constexpr const_reverse_iterator crbegin() const noexcept {
    return view_.crbegin();
  }
  constexpr const_reverse_iterator crend() const noexcept {
    return view_.crend();
  }

  // Capacity methods
  constexpr size_type size() const noexcept { return view_.size(); }
  constexpr size_type length() const noexcept { return view_.length(); }
  constexpr size_type max_size() const noexcept { return view_.max_size(); }
  constexpr bool empty() const noexcept { return view_.empty(); }

  // Modifiers
  constexpr void remove_prefix(size_type n) { view_.remove_prefix(n); }

  // String operations
  constexpr size_type find(view_type sv, size_type pos = 0) const noexcept {
    return view_.find(sv, pos);
  }
  constexpr size_type find(Char ch, size_type pos = 0) const noexcept {
    return view_.find(ch, pos);
  }
  constexpr size_type find(
      Char const* s, size_type pos, size_type count) const {
    return view_.find(s, pos, count);
  }
  constexpr size_type find(Char const* s, size_type pos = 0) const {
    return view_.find(s, pos);
  }

  constexpr size_type rfind(view_type sv, size_type pos = npos) const noexcept {
    return view_.rfind(sv, pos);
  }
  constexpr size_type rfind(Char ch, size_type pos = npos) const noexcept {
    return view_.rfind(ch, pos);
  }
  constexpr size_type rfind(
      Char const* s, size_type pos, size_type count) const {
    return view_.rfind(s, pos, count);
  }
  constexpr size_type rfind(Char const* s, size_type pos = npos) const {
    return view_.rfind(s, pos);
  }

  constexpr size_type find_first_of(
      view_type sv, size_type pos = 0) const noexcept {
    return view_.find_first_of(sv, pos);
  }
  constexpr size_type find_first_of(Char ch, size_type pos = 0) const noexcept {
    return view_.find_first_of(ch, pos);
  }
  constexpr size_type find_first_of(
      Char const* s, size_type pos, size_type count) const {
    return view_.find_first_of(s, pos, count);
  }
  constexpr size_type find_first_of(Char const* s, size_type pos = 0) const {
    return view_.find_first_of(s, pos);
  }

  constexpr size_type find_last_of(
      view_type sv, size_type pos = npos) const noexcept {
    return view_.find_last_of(sv, pos);
  }
  constexpr size_type find_last_of(
      Char ch, size_type pos = npos) const noexcept {
    return view_.find_last_of(ch, pos);
  }
  constexpr size_type find_last_of(
      Char const* s, size_type pos, size_type count) const {
    return view_.find_last_of(s, pos, count);
  }
  constexpr size_type find_last_of(Char const* s, size_type pos = npos) const {
    return view_.find_last_of(s, pos);
  }

  constexpr size_type find_first_not_of(
      view_type sv, size_type pos = 0) const noexcept {
    return view_.find_first_not_of(sv, pos);
  }
  constexpr size_type find_first_not_of(
      Char ch, size_type pos = 0) const noexcept {
    return view_.find_first_not_of(ch, pos);
  }
  constexpr size_type find_first_not_of(
      Char const* s, size_type pos, size_type count) const {
    return view_.find_first_not_of(s, pos, count);
  }
  constexpr size_type find_first_not_of(
      Char const* s, size_type pos = 0) const {
    return view_.find_first_not_of(s, pos);
  }

  constexpr size_type find_last_not_of(
      view_type sv, size_type pos = npos) const noexcept {
    return view_.find_last_not_of(sv, pos);
  }
  constexpr size_type find_last_not_of(
      Char ch, size_type pos = npos) const noexcept {
    return view_.find_last_not_of(ch, pos);
  }
  constexpr size_type find_last_not_of(
      Char const* s, size_type pos, size_type count) const {
    return view_.find_last_not_of(s, pos, count);
  }
  constexpr size_type find_last_not_of(
      Char const* s, size_type pos = npos) const {
    return view_.find_last_not_of(s, pos);
  }

  constexpr int compare(view_type sv) const noexcept {
    return view_.compare(sv);
  }
  constexpr int compare(size_type pos1, size_type count1, view_type sv) const {
    return view_.compare(pos1, count1, sv);
  }
  constexpr int compare(
      size_type pos1,
      size_type count1,
      view_type sv,
      size_type pos2,
      size_type count2) const {
    return view_.compare(pos1, count1, sv, pos2, count2);
  }
  constexpr int compare(Char const* s) const { return view_.compare(s); }
  constexpr int compare(size_type pos1, size_type count1, Char const* s) const {
    return view_.compare(pos1, count1, s);
  }
  constexpr int compare(
      size_type pos1, size_type count1, Char const* s, size_type count2) const {
    return view_.compare(pos1, count1, s, count2);
  }

  // Provide c_str() method - this is the key benefit
  constexpr Char const* c_str() const noexcept { return data(); }

  // Implicit conversion to string_view
  constexpr operator view_type() const noexcept { return view_; }

  // Provide safe substr that goes to end
  constexpr basic_cstring_view substr(size_type pos = 0) const {
    auto sub = view_.substr(pos);
    return {sub.data(), sub.size()};
  }

  constexpr void swap(basic_cstring_view& other) noexcept {
    view_.swap(other.view_);
  }

  constexpr bool starts_with(view_type sv) const noexcept {
    return view_.starts_with(sv);
  }
  constexpr bool starts_with(Char ch) const noexcept {
    return view_.starts_with(ch);
  }
  constexpr bool starts_with(Char const* s) const {
    return view_.starts_with(s);
  }

  constexpr bool ends_with(view_type sv) const noexcept {
    return view_.ends_with(sv);
  }
  constexpr bool ends_with(Char ch) const noexcept {
    return view_.ends_with(ch);
  }
  constexpr bool ends_with(Char const* s) const { return view_.ends_with(s); }

#if defined(__cpp_lib_string_contains) && __cpp_lib_string_contains >= 202011L
  constexpr bool contains(view_type sv) const noexcept {
    return view_.contains(sv);
  }
  constexpr bool contains(Char ch) const noexcept { return view_.contains(ch); }
  constexpr bool contains(Char const* s) const { return view_.contains(s); }
#endif

  // Explicitly delete dangerous operations that could break null-termination
  constexpr basic_cstring_view substr(size_type pos, size_type len) const =
      delete;
  constexpr void remove_suffix(size_type n) = delete;

  friend bool operator==(self lhs, self rhs) noexcept {
    return view_type(lhs) == view_type(rhs);
  }
  friend bool operator!=(self lhs, self rhs) noexcept {
    return view_type(lhs) != view_type(rhs);
  }
  friend bool operator<(self lhs, self rhs) noexcept {
    return view_type(lhs) < view_type(rhs);
  }
  friend bool operator<=(self lhs, self rhs) noexcept {
    return view_type(lhs) <= view_type(rhs);
  }
  friend bool operator>(self lhs, self rhs) noexcept {
    return view_type(lhs) > view_type(rhs);
  }
  friend bool operator>=(self lhs, self rhs) noexcept {
    return view_type(lhs) >= view_type(rhs);
  }
};

using cstring_view = basic_cstring_view<char>;
using wcstring_view = basic_cstring_view<wchar_t>;

template <typename Char, typename Traits>
std::basic_ostream<Char, Traits>& operator<<(
    std::basic_ostream<Char, Traits>& out,
    basic_cstring_view<Char, Traits> str) {
  return out << std::basic_string_view<Char, Traits>(str);
}

inline namespace literals {
inline namespace string_literals {

// User-defined literal for cstring_view
constexpr cstring_view operator""_csv(
    const char* str, std::size_t len) noexcept {
  return cstring_view(str, len);
}

} // namespace string_literals
} // namespace literals

} // namespace folly

// std::hash specialization
namespace std {
template <typename Char, typename Traits>
struct hash<folly::basic_cstring_view<Char, Traits>>
    : hash<std::basic_string_view<Char, Traits>> {};
} // namespace std

namespace fmt {
template <typename Char, typename Traits>
struct formatter<folly::basic_cstring_view<Char, Traits>, Char>
    : formatter<std::basic_string_view<Char, Traits>, Char> {};
} // namespace fmt
