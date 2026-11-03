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

#include <source_location>
#include <string_view>

#include <fmt/format.h>

#include <folly/Utility.h>
#include <folly/lang/Exception.h>
#include <folly/lang/SafeAssert.h>

namespace folly {

class rich_msg;

namespace ext { // For extension authors -- public details

/// ext::format_string_and_location
///
/// Rich error users probably want `rich_msg` instead, see below.  This is meant
/// to enable library authors to make concise APIs, in the style
/// `epitaph` or `...coded_rich_error`.
///
/// Captures a source location together with a literal string -- either for
/// `fmt` formatting with arguments, or one without substitutions. Usage:
/// template <typename... As> void
/// yourFn(format_string_and_location<std::type_identity_t<As>...> = "")
///
/// The `type_identity` allows implicit construction of this type from string
/// literals (its sole ctor is consteval).  Adding that nested template makes
/// the argument non-deducible.  Without that, template deduction would attempt
/// to deduce `const char [N]` -> `format_string_and_location`, which would fail
/// since template deduction does not consider implicit conversions.
///
/// BUG ALERT: Due to https://github.com/llvm/llvm-project/issues/137907 avoid
/// writing constructors like this: T(std::source_location sl =
/// std::source_location::current()) A constructor with a defaulted
/// `std::source_location` should take at least one mandatory argument to avoid
/// this pitfall.
template <typename... Args>
class format_string_and_location {
 private:
  friend class ::folly::rich_msg;

  std::source_location loc_;
  std::string_view fmt_str_;
  literal_c_str lit_str_;

 public:
  /* implicit */ consteval format_string_and_location(
      const char* str,
      std::source_location loc = std::source_location::current())
      : loc_{std::move(loc)}, fmt_str_{str}, lit_str_{str} {
    // fmt 12 makes `fmt::format_string` non-`constexpr` where it lacks
    // `consteval` (e.g. MSVC < 19.40); there, fmt itself checks at runtime.
    // TODO(T294126399): Drop `FMT_HAS_CONSTEVAL` (fmt 9) after the migration.
#if defined(FMT_HAS_CONSTEVAL) || \
    (defined(FMT_USE_CONSTEVAL) && FMT_USE_CONSTEVAL)
    [[maybe_unused]] fmt::format_string<Args...> check{str};
#endif
  }

  constexpr std::source_location location() const { return loc_; }

  constexpr exception_shared_string as_exception_shared_string(Args const&...)
    requires(sizeof...(Args) == 0)
  {
    return exception_shared_string{lit_str_};
  }

  exception_shared_string as_exception_shared_string(Args const&... args)
    requires(sizeof...(Args) > 0)
  {
    // `fmt::runtime` is safe since our ctor checked the string/args combo
    //
    // Future: If this code shows up in your benchmark, check out the test
    // `exception_shared_string_format` for a possible micro-optimization that
    // involves customizing the type given to `fmt` based on
    // `is_register_pass_v`.
    return {
        fmt::formatted_size(fmt::runtime(fmt_str_), args...),
        [&](auto buf, auto len) {
          auto res =
              fmt::format_to_n(buf, len, fmt::runtime(fmt_str_), args...);
          FOLLY_SAFE_DCHECK(len == res.size);
        }};
  }
};

} // namespace ext

/// A message that can automatically capture source locations. Usage:
///   rich_msg msg1{"error: {} at {}", code, line};
class rich_msg {
 private:
  exception_shared_string msg_;
  std::source_location loc_;

 public:
  // Writing `rich_error{"fmt {} str {}", 1, 2}` auto-captures the location.
  //
  // Future: Could make implicit. Would still need curly braces to construct:
  //  []() -> rich_msg { return "simple message"; } // error
  //  []() -> rich_msg { return {"simple message"}; } // ok
  template <typename... As>
  explicit rich_msg(
      ext::format_string_and_location<std::type_identity_t<As>...> snl,
      As const&... args)
      : msg_{snl.as_exception_shared_string(args...)},
        loc_{std::move(snl.loc_)} {}

  // This overload exists specifically for use in `immortal_rich_error`, which
  // passes all of the error's constructor arguments as template parameters.
  // In C++20, this imposes severe constraints.  We cannot pass:
  //   - `std::source_location` since it's not a structural type.
  //   - `rich_msg`, since it's not structural (and can likely never be).
  //   - String literals, or `literal_c_str` because `char*` is forbidden in
  //     template parameters.
  //   - `literal_string`, because that instance would not have static storage
  //     and thus we could use its `c_str()` in this `consteval` code.
  //   - Format args, since our `fmt` isn't `constexpr` (yet).
  //
  // Implicitly converting from a `"foo"_litv` is about as good as this can get.
  template <literal_string Str>
  /* implicit */ consteval rich_msg(vtag_t<Str>)
      : msg_{literal_c_str{Str.c_str()}}, loc_{} {}

  rich_msg(exception_shared_string msg, std::source_location loc)
      : msg_{std::move(msg)}, loc_{std::move(loc)} {}

  constexpr const std::source_location& location() const noexcept {
    return loc_;
  }
  constexpr const char* message() const noexcept { return msg_.what(); }
  exception_shared_string&& shared_string() && noexcept {
    return std::move(msg_);
  }
};

} // namespace folly
