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
#include <cstddef>
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
struct fmt_legacy_sizing_projection {
  using type = std::conditional_t<
      fmt_has_size_hint<T>,
      fmt_sizing_arg<T>,
      fmt_format_arg_t<T>>;

  static decltype(auto) make(T const& arg, fmt_sizing_state& state) {
    if constexpr (fmt_has_size_hint<T>) {
      return fmt_sizing_arg<T>{arg, state};
    } else {
      return fmt_format_arg_t<T>(arg);
    }
  }
};

template <
    typename T,
    template <typename> class Projection,
    bool = fmt::detail::is_named_arg<T>::value>
struct fmt_arg_store {
  typename Projection<T>::type value;

  fmt_arg_store(T const& arg, fmt_sizing_state& state)
      : value(Projection<T>::make(arg, state)) {}

  decltype(auto) get() const {
    return fmt_format_arg_t<std::remove_cvref_t<decltype(value)>>(value);
  }
};

template <typename T, template <typename> class Projection>
struct fmt_arg_store<T, Projection, true> {
  using value_type = std::remove_cvref_t<decltype(std::declval<T>().value)>;
  typename Projection<value_type>::type value;
  char const* name;

  fmt_arg_store(T const& arg, fmt_sizing_state& state)
      : value(Projection<value_type>::make(arg.value, state)), name(arg.name) {}

  auto get() const { return fmt::arg(name, value); }
};

#if FMT_USE_NONTYPE_TEMPLATE_ARGS
#if FMT_VERSION >= 110100
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

template <
    typename T,
    typename Char,
    size_t N,
    fmt_fixed_string<Char, N> Name,
    template <typename> class Projection>
struct fmt_arg_store<fmt_static_named_arg<T, Char, N, Name>, Projection, true> {
  using value_type = std::remove_cvref_t<T>;
  typename Projection<value_type>::type value;

  fmt_arg_store(
      fmt_static_named_arg<T, Char, N, Name> const& arg,
      fmt_sizing_state& state)
      : value(Projection<value_type>::make(arg.value, state)) {}

  auto get() const {
    using stored_type = std::remove_cvref_t<decltype(value)>;
    using named_type = fmt_static_named_arg<stored_type, Char, N, Name>;
    return named_type(value);
  }
};
#endif

namespace fmt_output {

template <typename Storage>
class fmt_format_output {
 public:
  using iterator_category = std::output_iterator_tag;
  using value_type = void;
  using difference_type = std::ptrdiff_t;
  using pointer = void;
  using reference = void;

  explicit fmt_format_output(Storage& out) : out_(&out) {}

  fmt_format_output& operator*() { return *this; }
  fmt_format_output& operator++() { return *this; }
  fmt_format_output operator++(int) { return *this; }
  fmt_format_output& operator=(char c) {
    if constexpr (std::is_same_v<Storage, fmt_sizing_state>) {
      out_->add(1);
    } else {
      out_->push_back(c);
    }
    return *this;
  }

  void add_size_hint(size_t size) const
    requires std::is_same_v<Storage, fmt_sizing_state>
  {
    out_->add(size);
  }

  void append(std::string_view str) const {
    if constexpr (std::is_same_v<Storage, fmt_sizing_state>) {
      out_->add(str.size());
    } else {
      out_->append(str);
    }
  }
  void append(char const* begin, char const* end) const {
    if (begin != end) {
      append(std::string_view(begin, static_cast<size_t>(end - begin)));
    }
  }

 private:
  Storage* out_;
};

// Bulk-copy hooks found by ADL in fmt's iterator-writing paths.
template <typename Char, typename InputChar, typename Storage>
  requires(
      std::is_same_v<Char, char> &&
      std::is_same_v<std::remove_const_t<InputChar>, char>)
auto copy(InputChar* begin, InputChar* end, fmt_format_output<Storage> out)
    -> fmt_format_output<Storage> {
  static_assert(std::is_same_v<Char, char>);
  out.append(begin, end);
  return out;
}

template <typename Char, typename InputChar, typename Storage>
  requires(
      std::is_same_v<Char, char> &&
      std::is_same_v<std::remove_const_t<InputChar>, char>)
auto copy_str(InputChar* begin, InputChar* end, fmt_format_output<Storage> out)
    -> fmt_format_output<Storage> {
  return copy<Char>(begin, end, out);
}

template <typename Char, typename InputChar, typename Storage>
  requires(
      std::is_same_v<Char, char> &&
      std::is_same_v<std::remove_const_t<InputChar>, char>)
auto copy_str_noinline(
    InputChar* begin, InputChar* end, fmt_format_output<Storage> out)
    -> fmt_format_output<Storage> {
  return copy<Char>(begin, end, out);
}

template <typename Char, typename UInt>
  requires std::is_same_v<Char, char>
auto format_base2e(
    int,
    fmt_format_output<fmt_sizing_state> out,
    UInt,
    int digits,
    bool = false) -> fmt_format_output<fmt_sizing_state> {
  out.add_size_hint(static_cast<size_t>(digits));
  return out;
}

#if FMT_VERSION >= 110100
template <typename Char, typename UInt>
  requires std::is_same_v<Char, char>
auto format_base2e(
    int base_bits,
    fmt_format_output<std::string> out,
    UInt value,
    int digits,
    bool upper = false) -> fmt_format_output<std::string> {
  char buffer[sizeof(UInt) * 8];
  auto const end =
      fmt::detail::format_base2e<Char>(base_bits, buffer, value, digits, upper);
  out.append(buffer, end);
  return out;
}
#endif

} // namespace fmt_output

using fmt_output::fmt_format_output;
using fmt_counting_context =
    fmt::basic_format_context<fmt_format_output<fmt_sizing_state>, char>;

template <typename Context>
concept fmt_counting_format_context =
    std::is_same_v<Context, fmt_counting_context>;

struct fmt_sizing_handler {
  using argument = fmt::basic_format_arg<fmt_counting_context>;
  using custom_handle = argument::handle;

  fmt::format_parse_context parse_context;
  fmt_counting_context context;

  fmt_sizing_handler(
      fmt::string_view spec,
      fmt::basic_format_args<fmt_counting_context> args,
      fmt_sizing_state& state)
      : parse_context(spec), context(fmt_format_output(state), args) {}

  void on_text(char const* begin, char const* end) {
    context.out().append(begin, end);
  }

  auto on_arg_id() -> int { return parse_context.next_arg_id(); }
  auto on_arg_id(int id) -> int {
    parse_context.check_arg_id(id);
    return id;
  }
  auto on_arg_id(fmt::string_view name) -> int {
    parse_context.check_arg_id(name);
    auto const id = context.arg_id(name);
    if (id < 0) {
      on_error("argument not found");
    }
    return id;
  }

  void on_replacement_field(int id, char const*) {
    fmt::format_parse_context empty(fmt::string_view{});
    format_field(id, empty);
  }

  auto on_format_specs(int id, char const* begin, char const*) -> char const* {
    parse_context.advance_to(begin);
    return format_field(id, parse_context);
  }

  auto format_field(int id, fmt::format_parse_context& parse) -> char const* {
    auto const arg = context.arg(id);
    if (!arg) {
      on_error("argument not found");
    }
    auto const format = [&](auto const& value) -> char const* {
      using value_type = std::remove_cvref_t<decltype(value)>;
      if constexpr (std::is_same_v<value_type, fmt::monostate>) {
        on_error("argument not found");
      } else if constexpr (std::is_same_v<value_type, custom_handle>) {
        value.format(parse, context);
        return parse.begin();
      } else {
        fmt::formatter<value_type> formatter;
        auto const end = formatter.parse(parse);
        context.advance_to(formatter.format(value, context));
        return end;
      }
    };
#if FMT_VERSION >= 110000
    return arg.visit(format);
#else
    return fmt::visit_format_arg(format, arg);
#endif
  }

  [[noreturn]] void on_error(char const* message) const {
    throw_exception<fmt::format_error>(message);
  }
};

template <typename Handler>
void fmt_parse_format_string(fmt::string_view spec, Handler& handler) {
  if constexpr (requires { fmt::detail::parse_format_string(spec, handler); }) {
    fmt::detail::parse_format_string(spec, handler); // NOLINT
  } else {
    fmt::detail::parse_format_string<false>(spec, handler); // NOLINT
  }
}

template <typename... Args>
auto fmt_generic_formatted_size(fmt::string_view spec, Args const&... args)
    -> size_t {
  auto const measure = [&](fmt_format_arg_t<Args>... values) {
    auto const format_args =
        fmt::make_format_args<fmt_counting_context>(values...);
    fmt_sizing_state state;
    fmt_sizing_handler handler(spec, format_args, state);
    fmt_parse_format_string(spec, handler);
    return state.offset;
  };
  return measure(args...);
}

template <typename Storage>
class fmt_output_buffer final : public fmt::detail::buffer<char> {
 public:
  // NOLINTNEXTLINE
  explicit fmt_output_buffer(Storage& out)
      : fmt::detail::buffer<char>(
#if FMT_VERSION >= 110000
            grow,
#endif
            buffer_,
            0,
            sizeof(buffer_)),
        out_(out) {
  }

  ~fmt_output_buffer() = default;
  fmt_output_buffer(fmt_output_buffer const&) = delete;
  fmt_output_buffer& operator=(fmt_output_buffer const&) = delete;
  fmt_output_buffer(fmt_output_buffer&&) = delete;
  fmt_output_buffer& operator=(fmt_output_buffer&&) = delete;

  void flush() {
    auto const size = this->size(); // NOLINT
    if (size != 0) {
      out_.append(std::string_view(this->data(), size));
      this->clear(); // NOLINT
    }
  }

 private:
#if FMT_VERSION >= 110000
  static void grow(fmt::detail::buffer<char>& buffer, size_t) {
    static_cast<fmt_output_buffer&>(buffer).flush();
  }
#else
  void grow(size_t) override { flush(); }
#endif

  char buffer_[256];
  Storage& out_;
};

template <typename Storage, typename... Args>
void fmt_format_to(
    Storage& out,
    fmt::format_string<fmt_format_arg_t<Args>...> spec,
    Args const&... args) {
  fmt_output_buffer buffer(out);
  fmt::format_to(fmt::appender(buffer), spec, fmt_format_arg_t<Args>(args)...);
  buffer.flush();
}

template <typename Storage, fmt_compiled_string Format, typename... Args>
void fmt_format_to(Storage& out, Format const& spec, Args const&... args) {
  fmt::format_to(fmt_format_output(out), spec, fmt_format_arg_t<Args>(args)...);
}

template <typename OutputIt>
class fmt_output_sink {
 public:
  explicit fmt_output_sink(OutputIt out) : out_(out) {}

  void push_back(char c) { *out_++ = c; }

  void append(std::string_view str) {
    if (str.empty()) {
      return;
    }
    if constexpr (can_append_direct) {
      out_.append(str);
    } else if constexpr (can_append_view) {
      fmt::detail::get_container(out_).append(str);
    } else if constexpr (can_append_range) {
      fmt::detail::get_container(out_).append(str.begin(), str.end());
    } else {
      out_ = fmt::format_to(out_, FOLLY_FMT_COMPILE("{}"), str);
    }
  }

  OutputIt out() const { return out_; }

 private:
  static constexpr bool can_append_direct =
      requires(OutputIt out, std::string_view str) { out.append(str); };
#if FMT_VERSION >= 120000 || FMT_VERSION < 100000
  static constexpr bool can_append_view = requires(
      OutputIt out, std::string_view str) {
    fmt::detail::get_container(out).append(str);
  };
  static constexpr bool can_append_range = requires(
      OutputIt out, std::string_view str) {
    fmt::detail::get_container(out).append(str.begin(), str.end());
  };
#else
  // fmt 10 and 11 declare get_container() for appenders but define it in
  // terms of a container member that appenders lack there
  static constexpr bool can_append_view = false;
  static constexpr bool can_append_range = false;
#endif

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
    if constexpr (
        detail::fmt_counting_format_context<Context> &&
        detail::fmt_has_size_hint<T>) {
      auto const size = static_cast<Derived const&>(*this).size_hint(value);
      ctx.out().add_size_hint(size);
      return ctx.out();
    } else {
      detail::fmt_output_sink sink(ctx.out());
      static_cast<Derived const&>(*this).append(value, sink);
      return sink.out();
    }
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
  FOLLY_ALWAYS_INLINE auto format(
      folly::detail::fmt_sizing_arg<T> const& arg, Context& ctx) const {
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
/// Checked/runtime sizing uses fmt's parser and a generic counting context,
/// preserving original argument types and native formatter dispatch. Character
/// and range writes only advance the count; no scratch output is generated.
/// Custom formatters must accept a generic format context. There is no buffered
/// fallback for formatters requiring exactly fmt::format_context.
/// Compiled formats retain their compiled counting path.
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
    if constexpr (detail::fmt_compiled_string<Format>) {
      detail::fmt_sizing_state state;
      auto const measure = [&](auto const&... stored) {
        fmt::format_to(detail::fmt_format_output(state), spec, stored.get()...);
      };
      measure(
          detail::fmt_arg_store<Args, detail::fmt_legacy_sizing_projection>(
              args, state)...);
      return state.offset;
    } else {
      if constexpr (requires { spec.get(); }) {
        return detail::fmt_generic_formatted_size(spec.get(), args...);
      } else {
        return detail::fmt_generic_formatted_size(spec, args...);
      }
    }
  }

  template <typename Format, typename... Args>
  static std::string format(Format const& spec, Args const&... args) {
    std::string out;
    out.reserve(size(spec, args...));
    detail::fmt_format_to(out, spec, args...);
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
