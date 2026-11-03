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

#include <concepts>
#include <iterator>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include <fmt/args.h>
#include <fmt/format.h>

#include <folly/CppAttributes.h>
#include <folly/lang/Exception.h>
#include <folly/portability/FmtCompile.h>

namespace folly {

namespace detail {
template <typename T>
concept fmt_compiled_string =
#if FMT_VERSION >= 110104
    fmt::is_compiled_string<T>::value;
#else
    fmt::detail::is_compiled_string<T>::value;
#endif

struct fmt_sized_formatter_base_tag {};

template <typename T>
inline constexpr bool fmt_has_size_hint =
    std::is_base_of_v<fmt_sized_formatter_base_tag, fmt::formatter<T>> &&
    !std::is_convertible_v<T, fmt::string_view> &&
    requires(fmt::formatter<T> const& formatter, T const& value) {
      { formatter.size_hint(value) } -> std::convertible_to<size_t>;
    };

template <typename T>
inline constexpr bool fmt_needs_sizing_arg = [] {
  if constexpr (fmt::detail::is_named_arg<T>::value) {
    using value_type = std::remove_cvref_t<decltype(std::declval<T>().value)>;
    return fmt_has_size_hint<value_type>;
  } else {
    return fmt_has_size_hint<T>;
  }
}();

struct fmt_sizing_state {
  size_t offset = 0;

  void add(size_t size) {
    if (size > std::string{}.max_size() - offset) {
      throw_exception<std::length_error>(
          "formatted size exceeds string max_size");
    }
    offset += size;
  }
};

template <typename T>
struct fmt_sizing_arg {
  T const& value;
  fmt_sizing_state& state;
};

template <typename T>
inline constexpr bool fmt_arg_by_value = fmt::detail::is_named_arg<T>::value ||
    std::is_base_of_v<fmt::detail::view, T>;

template <typename T>
using fmt_format_arg_t = std::conditional_t<fmt_arg_by_value<T>, T, T const&>;

template <typename T>
using fmt_sizing_arg_t = std::
    conditional_t<fmt_has_size_hint<T>, fmt_sizing_arg<T>, fmt_format_arg_t<T>>;

template <typename T>
decltype(auto) fmt_make_sizing_arg(T const& arg, fmt_sizing_state& state) {
  if constexpr (fmt_has_size_hint<T>) {
    return fmt_sizing_arg<T>{arg, state};
  } else {
    return fmt_format_arg_t<T>(arg);
  }
}

template <typename T, bool = fmt::detail::is_named_arg<T>::value>
struct fmt_sizing_arg_store {
  fmt_sizing_arg_t<T> value;

  fmt_sizing_arg_store(T const& arg, fmt_sizing_state& state)
      : value(fmt_make_sizing_arg(arg, state)) {}

  decltype(auto) get() const {
    return fmt_format_arg_t<std::remove_cvref_t<decltype(value)>>(value);
  }
};

template <typename T>
struct fmt_sizing_arg_store<T, true> {
  using value_type = std::remove_cvref_t<decltype(std::declval<T>().value)>;
  fmt_sizing_arg_t<value_type> value;
  char const* name;

  fmt_sizing_arg_store(T const& arg, fmt_sizing_state& state)
      : value(fmt_make_sizing_arg(arg.value, state)), name(arg.name) {}

  auto get() const { return fmt::arg(name, value); }
};

#if FMT_USE_NONTYPE_TEMPLATE_ARGS
#if FMT_VERSION >= 120000
template <typename Char, size_t N>
using fmt_fixed_string = fmt::detail::fixed_string<Char, N>;
template <typename T, typename Char, size_t N, fmt_fixed_string<Char, N> Name>
using fmt_static_named_arg = fmt::detail::static_named_arg<T, Char, N, Name>;
#else
template <typename Char, size_t N>
using fmt_fixed_string = fmt::detail_exported::fixed_string<Char, N>;
template <typename T, typename Char, size_t N, fmt_fixed_string<Char, N> Name>
using fmt_static_named_arg =
    fmt::detail::statically_named_arg<T, Char, N, Name>;
#endif

template <typename T, typename Char, size_t N, fmt_fixed_string<Char, N> Name>
struct fmt_sizing_arg_store<fmt_static_named_arg<T, Char, N, Name>, true> {
  using value_type = std::remove_cvref_t<T>;
  fmt_sizing_arg_t<value_type> value;

  fmt_sizing_arg_store(
      fmt_static_named_arg<T, Char, N, Name> const& arg,
      fmt_sizing_state& state)
      : value(fmt_make_sizing_arg(arg.value, state)) {}

  auto get() const {
    using stored_type = std::remove_cvref_t<decltype(value)>;
    using named_type = fmt_static_named_arg<stored_type, Char, N, Name>;
    return named_type(value);
  }
};
#endif

template <typename OutputIt>
class fmt_output_sink {
 public:
  explicit fmt_output_sink(OutputIt out) : out_(out) {}

  void push_back(char c) { *out_++ = c; }

  void append(std::string_view str) {
    if (str.empty()) {
      return;
    }
    if constexpr (can_append_view) {
      fmt::detail::get_container(out_).append(str);
    } else if constexpr (can_append_range) {
      fmt::detail::get_container(out_).append(str.begin(), str.end());
    } else {
      out_ = fmt::format_to(out_, FOLLY_FMT_COMPILE("{}"), str);
    }
  }

  OutputIt out() const { return out_; }

 private:
  static constexpr bool can_append_view = requires(
      OutputIt out, std::string_view str) {
    fmt::detail::get_container(out).append(str);
  };
  static constexpr bool can_append_range = requires(
      OutputIt out, std::string_view str) {
    fmt::detail::get_container(out).append(str.begin(), str.end());
  };

  OutputIt out_;
};

} // namespace detail

/// A base for fmt::formatter specializations which separate sizing from
/// writing. Derived supplies size_hint(value) and append(value, sink), both
/// callable on a const formatter. The sink supports push_back(char) and
/// append(string_view). The default parse accepts only an empty format
/// specifier.
///
/// For values without an implicit conversion to fmt::string_view, ordinary
/// fmt::format and fmt::format_to calls use append, and fmt_sized_format uses
/// size_hint on its first pass, also for runtime formats. Hints may under- or
/// overestimate the output; they reserve capacity without limiting writes.
/// Implicitly string-convertible values retain fmt's native dispatch instead:
/// compiled fmt 12 may use the conversion rather than the custom formatter.
/// Arguments and referenced data must outlive the formatting call.
///
/// For example:
///   template <>
///   struct fmt::formatter<Value>
///       : folly::fmt_sized_formatter_base<fmt::formatter<Value>> {
///     static size_t size_hint(Value const& value);
///     template <typename Sink>
///     static void append(Value const& value, Sink& sink);
///   };
template <typename Derived>
struct fmt_sized_formatter_base : detail::fmt_sized_formatter_base_tag {
  constexpr auto parse(fmt::format_parse_context& ctx) {
    auto const it = ctx.begin();
    if (it != ctx.end() && *it != '}') {
      throw_exception<fmt::format_error>("unsupported format specifier");
    }
    return it;
  }

  template <typename T, typename Context>
  auto format(T const& value, Context& ctx) const {
    detail::fmt_output_sink sink(ctx.out());
    static_cast<Derived const&>(*this).append(value, sink);
    return sink.out();
  }
};
} // namespace folly

template <typename T>
struct fmt::formatter<folly::detail::fmt_sizing_arg<T>> {
  fmt::formatter<T> impl;

  constexpr auto parse(fmt::format_parse_context& ctx) {
    return impl.parse(ctx);
  }

  template <typename Context>
  auto format(folly::detail::fmt_sizing_arg<T> const& arg, Context& ctx) const {
    arg.state.add(impl.size_hint(arg.value));
    return ctx.out();
  }
};

namespace folly {

/// fmt_sized_format_fn
/// fmt_sized_format
///
/// Two-pass formatting: measure the output, reserve the final std::string,
/// then format into it. Accepts checked, runtime, and FOLLY_FMT_COMPILE format
/// strings.
///
/// The size pass reserves capacity, not a limit on the output. The string
/// grows as needed if a custom formatter underestimates the output size.
/// Values are passed by const reference, without moves.
/// Fmt view wrappers are copied for fmt's argument bookkeeping.
///
/// Prefer compiled formats to reduce the cost of the sizing pass. Two-pass
/// formatting is not necessarily faster than fmt::format for every workload.
struct fmt_sized_format_fn {
 private:
  template <typename T>
  static constexpr bool format_arg_by_value =
      fmt::detail::is_named_arg<T>::value ||
      std::is_base_of_v<fmt::detail::view, T>;

  template <typename T>
  using format_arg_t = std::conditional_t<format_arg_by_value<T>, T, T const&>;

 public:
  template <typename... Args>
  std::string operator()(
      fmt::format_string<format_arg_t<Args>...> spec,
      Args const&... args) const {
    return format(spec, args...);
  }

  template <detail::fmt_compiled_string Format, typename... Args>
  std::string operator()(Format const& spec, Args const&... args) const {
    return format(spec, args...);
  }

 private:
  template <typename Format, typename... Args>
  static size_t size(Format const& spec, Args const&... args) {
    if constexpr ((detail::fmt_needs_sizing_arg<Args> || ...)) {
      detail::fmt_sizing_state state;
      auto const measure = [&](auto const&... stored) {
        if constexpr (detail::fmt_compiled_string<Format>) {
          return fmt::formatted_size(spec, stored.get()...);
        } else if constexpr (requires { spec.get(); }) {
          return fmt::formatted_size(fmt::runtime(spec.get()), stored.get()...);
        } else {
          return fmt::formatted_size(fmt::runtime(spec), stored.get()...);
        }
      };
      auto const written =
          measure(detail::fmt_sizing_arg_store<Args>(args, state)...);
      state.add(written);
      return state.offset;
    } else {
      return fmt::formatted_size(spec, format_arg_t<Args>(args)...);
    }
  }

  template <typename Format, typename... Args>
  static std::string format(Format const& spec, Args const&... args) {
    std::string out;
    out.reserve(size(spec, args...));
    fmt::format_to(std::back_inserter(out), spec, format_arg_t<Args>(args)...);
    return out;
  }
};
inline constexpr fmt_sized_format_fn fmt_sized_format{};

/// fmt_make_format_args_from_map_fn
/// fmt_make_format_args_from_map
///
/// A helper function-object type and variable for making a format-args object
/// from a map.
///
/// May be useful for transitioning from legacy folly::svformat to fmt::vformat.
struct fmt_make_format_args_from_map_fn {
  template <typename Map>
  fmt::dynamic_format_arg_store<fmt::format_context> operator()(
      [[FOLLY_ATTR_CLANG_LIFETIMEBOUND]] Map const& map) const {
    fmt::dynamic_format_arg_store<fmt::format_context> ret;
    ret.reserve(map.size(), map.size());
    for (auto const& [key, val] : map) {
      ret.push_back(fmt::arg(key.c_str(), std::cref(val)));
    }
    return ret;
  }
};
inline constexpr fmt_make_format_args_from_map_fn
    fmt_make_format_args_from_map{};

/// fmt_vformat_mangle_name_fn
/// fmt_vformat_mangle_name
///
/// A helper function-object type and variable for mangling vformat named-arg
/// names which fmt::vformat might not otherwise permit.
struct fmt_vformat_mangle_name_fn {
  std::string operator()(std::string_view const str) const;
  void operator()(std::string& out, std::string_view const str) const;
};
inline constexpr fmt_vformat_mangle_name_fn fmt_vformat_mangle_name{};

/// fmt_vformat_mangle_format_string_fn
/// fmt_vformat_mangle_format_string
///
/// A helper function-object type and variable for mangling the content of
/// vformat format-strings containing named-arg names which fmt::vformat might
/// not otherwise permit.
struct fmt_vformat_mangle_format_string_fn {
  struct options {
    bool numeric_args_as_named = false;

    options& set_numeric_args_as_named(bool value) noexcept {
      numeric_args_as_named = value;
      return *this;
    }
  };

  std::string operator()(std::string_view const str) const;
  std::string operator()(options const& opts, std::string_view const str) const;
};
inline constexpr fmt_vformat_mangle_format_string_fn
    fmt_vformat_mangle_format_string{};

using fmt_vformat_mangle_format_string_options =
    fmt_vformat_mangle_format_string_fn::options;

} // namespace folly
