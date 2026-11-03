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

#include <folly/FmtUtility.h>

#include <array>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/ranges.h>
#include <gtest/gtest.h>

using namespace std::literals;

namespace {
struct CustomValue {
  std::string_view value;

  explicit CustomValue(std::string_view v) : value(v) {}
  CustomValue(CustomValue const&) = delete;
  CustomValue& operator=(CustomValue const&) = delete;
};

struct GeneratedText {
  std::string_view text;
  size_t hint;
  unsigned& sizeCalls;
  unsigned& appendCalls;

  GeneratedText(std::string_view t, size_t h, unsigned& s, unsigned& a)
      : text(t), hint(h), sizeCalls(s), appendCalls(a) {}
  GeneratedText(GeneratedText const&) = delete;
  GeneratedText& operator=(GeneratedText const&) = delete;
};

struct FinalText {
  GeneratedText const& value;
};

struct ForwardedText {
  GeneratedText const& value;
};

struct StringLikeText {
  GeneratedText const& value;
  operator fmt::string_view() const {
    return {value.text.data(), value.text.size()};
  }
};

struct ChangingSize {
  std::string_view first;
  std::string_view second;
  mutable unsigned calls = 0;
};
} // namespace

template <>
struct fmt::formatter<CustomValue> : fmt::formatter<std::string_view> {
  template <typename Context>
  auto format(CustomValue const& value, Context& ctx) const {
    return fmt::formatter<std::string_view>::format(value.value, ctx);
  }
};

template <>
struct fmt::formatter<ChangingSize> : fmt::formatter<std::string_view> {
  template <typename Context>
  auto format(ChangingSize const& value, Context& ctx) const {
    return fmt::formatter<std::string_view>::format(
        value.calls++ == 0 ? value.first : value.second, ctx);
  }
};

template <>
struct fmt::formatter<GeneratedText>
    : folly::fmt_sized_formatter_base<fmt::formatter<GeneratedText>> {
  static size_t size_hint(GeneratedText const& value) {
    ++value.sizeCalls;
    return value.hint;
  }

  template <typename Sink>
  static void append(GeneratedText const& value, Sink& sink) {
    ++value.appendCalls;
    sink.push_back('[');
    sink.append(value.text);
    sink.push_back(']');
  }
};

template <>
struct fmt::formatter<FinalText> final
    : folly::fmt_sized_formatter_base<fmt::formatter<FinalText>> {
  static size_t size_hint(FinalText const& value) {
    return fmt::formatter<GeneratedText>::size_hint(value.value);
  }

  template <typename Sink>
  static void append(FinalText const& value, Sink& sink) {
    fmt::formatter<GeneratedText>::append(value.value, sink);
  }
};

template <>
struct fmt::formatter<ForwardedText> : fmt::formatter<GeneratedText> {
  template <typename Context>
  auto format(ForwardedText const& value, Context& ctx) const {
    return fmt::formatter<GeneratedText>::format(value.value, ctx);
  }
};

template <>
struct fmt::formatter<StringLikeText>
    : folly::fmt_sized_formatter_base<fmt::formatter<StringLikeText>> {
  static size_t size_hint(StringLikeText const& value) {
    return fmt::formatter<GeneratedText>::size_hint(value.value);
  }

  template <typename Sink>
  static void append(StringLikeText const& value, Sink& sink) {
    fmt::formatter<GeneratedText>::append(value.value, sink);
  }
};

struct FmtUtilityTest : testing::Test {};

TEST_F(FmtUtilityTest, formatter_base_size_and_append) {
  for (auto const length : {0, 1, 23, 500, 4096}) {
    std::string const text(length, 'x');
    auto const expected = "[" + text + "]";
    for (auto const hint : {0, 1, 32, 8192}) {
      unsigned sized = 0;
      unsigned appended = 0;
      GeneratedText const value(text, hint, sized, appended);
      EXPECT_EQ(expected, folly::fmt_sized_format("{}", value));
      EXPECT_EQ(1, sized);
      EXPECT_EQ(1, appended);
      sized = appended = 0;
      EXPECT_EQ(expected, folly::fmt_sized_format(fmt::runtime("{}"), value));
      EXPECT_EQ(1, sized);
      EXPECT_EQ(1, appended);
      sized = appended = 0;
      EXPECT_EQ(
          expected, folly::fmt_sized_format(FOLLY_FMT_COMPILE("{}"), value));
      EXPECT_EQ(1, sized);
      EXPECT_EQ(1, appended);
    }
  }
}

TEST_F(FmtUtilityTest, formatter_base_native_calls_append_only) {
  unsigned sized = 0;
  unsigned appended = 0;
  GeneratedText const value("a\0b"sv, 0, sized, appended);
  EXPECT_EQ("[a\0b]"s, fmt::format("{}", value));
  EXPECT_EQ("[a\0b]"s, fmt::format(fmt::runtime("{}"), value));
  EXPECT_EQ("[a\0b]"s, fmt::format(FOLLY_FMT_COMPILE("{}"), value));
  std::string out;
  fmt::format_to(std::back_inserter(out), "{}", value);
  EXPECT_EQ("[a\0b]"s, out);
  std::array<char, 5> raw{};
  auto end = fmt::format_to(raw.data(), FOLLY_FMT_COMPILE("{}"), value);
  EXPECT_EQ(raw.data() + raw.size(), end);
  EXPECT_EQ("[a\0b]"s, std::string(raw.data(), raw.size()));
  EXPECT_EQ(0, sized);
  EXPECT_EQ(5, appended);
}

TEST_F(FmtUtilityTest, formatter_base_mixed_and_named_arguments) {
  unsigned sized = 0;
  unsigned appended = 0;
  GeneratedText const value("text", 4, sized, appended);
  EXPECT_EQ(
      "001f [text] x    ",
      folly::fmt_sized_format("{:04x} {} {:<{}}", 31, value, "x", 5));
  EXPECT_EQ(
      "001f [text] x    ",
      folly::fmt_sized_format(
          FOLLY_FMT_COMPILE("{:04x} {} {:<{}}"), 31, value, "x", 5));
  EXPECT_EQ(
      "[text] 31",
      folly::fmt_sized_format(
          "{text} {num}", fmt::arg("text", value), fmt::arg("num", 31)));
  EXPECT_EQ(
      "[text] 31",
      folly::fmt_sized_format(
          fmt::runtime("{text} {num}"),
          fmt::arg("text", value),
          fmt::arg("num", 31)));
  EXPECT_EQ(
      "[text] 31",
      folly::fmt_sized_format(
          FOLLY_FMT_COMPILE("{text} {num}"),
          fmt::arg("text", value),
          fmt::arg("num", 31)));
  EXPECT_EQ("[text] [text]", folly::fmt_sized_format("{0} {0}", value));
  EXPECT_EQ(7, sized);
  EXPECT_EQ(7, appended);
}

TEST_F(FmtUtilityTest, formatter_base_static_named_arguments) {
  using namespace fmt::literals;
  unsigned sized = 0;
  unsigned appended = 0;
  GeneratedText const value("text", 4, sized, appended);
#if FMT_USE_NONTYPE_TEMPLATE_ARGS && !defined(_MSC_VER) && \
    !(defined(__GNUC__) && !defined(__clang__) && __GNUC__ <= 8)
  EXPECT_EQ(
      "[text] 001f",
      folly::fmt_sized_format(
          FOLLY_FMT_COMPILE("{text} {:04x}"), "text"_a = value, 31));
  EXPECT_EQ(
      "[text] 001f",
      folly::fmt_sized_format(
          FOLLY_FMT_COMPILE("{text:} {:04x}"), "text"_a = value, 31));
#else
  EXPECT_EQ(
      "[text] 001f",
      folly::fmt_sized_format(
          FOLLY_FMT_COMPILE("{text} {1:04x}"), "text"_a = value, 31));
  EXPECT_EQ(
      "[text] 001f",
      folly::fmt_sized_format(
          FOLLY_FMT_COMPILE("{0:} {1:04x}"), "text"_a = value, 31));
#endif
  EXPECT_EQ(
      "[text] 31",
      folly::fmt_sized_format("{text} {num}", "text"_a = value, "num"_a = 31));
  EXPECT_EQ(
      "[text] 31",
      folly::fmt_sized_format(
          fmt::runtime("{text} {num}"), "text"_a = value, "num"_a = 31));
  EXPECT_EQ(4, sized);
  EXPECT_EQ(4, appended);
}

TEST_F(FmtUtilityTest, formatter_base_final_and_forwarding_formatters) {
  unsigned sized = 0;
  unsigned appended = 0;
  GeneratedText const value("text", 4, sized, appended);
  FinalText const final{value};
  EXPECT_EQ("[text]", folly::fmt_sized_format("{}", final));
  EXPECT_EQ("[text]", folly::fmt_sized_format(FOLLY_FMT_COMPILE("{}"), final));
  EXPECT_EQ(2, sized);
  EXPECT_EQ(2, appended);
  sized = appended = 0;
  ForwardedText const forwarded{value};
  EXPECT_EQ("[text]", folly::fmt_sized_format("{}", forwarded));
  EXPECT_EQ(
      "[text]", folly::fmt_sized_format(FOLLY_FMT_COMPILE("{}"), forwarded));
  EXPECT_EQ(0, sized);
  EXPECT_EQ(4, appended);
}

TEST_F(
    FmtUtilityTest,
    formatter_base_string_conversion_preserves_native_dispatch) {
  unsigned sized = 0;
  unsigned appended = 0;
  GeneratedText const value("text", 0, sized, appended);
  StringLikeText const stringLike{value};
  EXPECT_EQ(
      fmt::format("x{}", stringLike),
      folly::fmt_sized_format("x{}", stringLike));
  EXPECT_EQ(
      fmt::format(fmt::runtime("x{}"), stringLike),
      folly::fmt_sized_format(fmt::runtime("x{}"), stringLike));
  EXPECT_EQ(
      fmt::format(FOLLY_FMT_COMPILE("x{}"), stringLike),
      folly::fmt_sized_format(FOLLY_FMT_COMPILE("x{}"), stringLike));
  EXPECT_EQ(0, sized);
}

TEST_F(FmtUtilityTest, formatter_base_size_overflow) {
  unsigned sized = 0;
  unsigned appended = 0;
  GeneratedText const value("text", std::string{}.max_size(), sized, appended);
  EXPECT_THROW(folly::fmt_sized_format("x{}", value), std::length_error);
  EXPECT_THROW(
      folly::fmt_sized_format(FOLLY_FMT_COMPILE("x{}"), value),
      std::length_error);
  EXPECT_EQ(2, sized);
  EXPECT_EQ(0, appended);
  EXPECT_EQ("[text]", fmt::format("{}", value));
}

TEST_F(FmtUtilityTest, formatter_base_invalid_specifiers) {
  unsigned sized = 0;
  unsigned appended = 0;
  GeneratedText const value("text", 4, sized, appended);
  EXPECT_THROW(
      folly::fmt_sized_format(fmt::runtime("{:x}"), value), fmt::format_error);
  EXPECT_THROW(
      (void)fmt::format(fmt::runtime("{:x}"), value), fmt::format_error);
  EXPECT_EQ("[text]", folly::fmt_sized_format(FOLLY_FMT_COMPILE("{:}"), value));
  EXPECT_EQ(1, sized);
  EXPECT_EQ(1, appended);
}

TEST_F(FmtUtilityTest, fmt_sized_format_checked) {
  EXPECT_EQ("", folly::fmt_sized_format(""));
  EXPECT_EQ("checked", folly::fmt_sized_format(FMT_STRING("{}"), "checked"));
  EXPECT_EQ("literal {text}", folly::fmt_sized_format("literal {{text}}"));
  EXPECT_EQ(
      "hello world 42", folly::fmt_sized_format("hello {} {}", "world", 42));
  EXPECT_EQ("001f 1.25", folly::fmt_sized_format("{:04x} {:.2f}", 31, 1.25));
  EXPECT_EQ("x    ", folly::fmt_sized_format("{:<{}}", "x", 5));
  EXPECT_EQ(
      "second first", folly::fmt_sized_format("{1} {0}", "first", "second"));
  EXPECT_EQ(
      "hello bob",
      folly::fmt_sized_format("hello {name}", fmt::arg("name", "bob")));
}

TEST_F(FmtUtilityTest, fmt_sized_format_compiled) {
  EXPECT_EQ("", folly::fmt_sized_format(FOLLY_FMT_COMPILE("")));
  EXPECT_EQ(
      "literal {text}",
      folly::fmt_sized_format(FOLLY_FMT_COMPILE("literal {{text}}")));
  EXPECT_EQ(
      "hello world 42",
      folly::fmt_sized_format(FOLLY_FMT_COMPILE("hello {} {}"), "world", 42));
  EXPECT_EQ(
      "001f 1.25",
      folly::fmt_sized_format(FOLLY_FMT_COMPILE("{:04x} {:.2f}"), 31, 1.25));
  EXPECT_EQ(
      "x    ", folly::fmt_sized_format(FOLLY_FMT_COMPILE("{:<{}}"), "x", 5));
}

TEST_F(FmtUtilityTest, fmt_sized_format_runtime) {
  std::string const spec = "hello {} {}";
  EXPECT_EQ(
      "hello world 42",
      folly::fmt_sized_format(fmt::runtime(spec), "world", 42));
  EXPECT_THROW(folly::fmt_sized_format(fmt::runtime("{")), fmt::format_error);
  EXPECT_THROW(folly::fmt_sized_format(fmt::runtime("{}")), fmt::format_error);
  EXPECT_THROW(
      folly::fmt_sized_format(fmt::runtime("{:q}"), 42), fmt::format_error);
}

TEST_F(FmtUtilityTest, fmt_sized_format_string_lengths) {
  for (auto const size : {0, 1, 15, 16, 22, 23, 24, 499, 500, 501, 4096}) {
    std::string const value(size, 'x');
    auto const expected = "[" + value + "]";
    EXPECT_EQ(expected, folly::fmt_sized_format("[{}]", value));
    EXPECT_EQ(
        expected, folly::fmt_sized_format(FOLLY_FMT_COMPILE("[{}]"), value));
  }
  std::string const value = "a\0b"s;
  EXPECT_EQ("[a\0b]"s, folly::fmt_sized_format("[{}]", value));
  EXPECT_EQ(
      "[a\0b]"s, folly::fmt_sized_format(FOLLY_FMT_COMPILE("[{}]"), value));
}

TEST_F(FmtUtilityTest, fmt_sized_format_custom_formatter) {
  CustomValue const value("custom");
  EXPECT_EQ("[custom]", folly::fmt_sized_format("[{}]", value));
  EXPECT_EQ(
      "[custom]", folly::fmt_sized_format(FOLLY_FMT_COMPILE("[{}]"), value));
  EXPECT_EQ("custom", folly::fmt_sized_format("{}", CustomValue("custom")));
  EXPECT_EQ(
      "custom",
      folly::fmt_sized_format(FOLLY_FMT_COMPILE("{}"), CustomValue("custom")));
}

TEST_F(FmtUtilityTest, fmt_sized_format_named_arguments) {
  CustomValue const value("custom");
  auto const named = fmt::arg("name", value);
  EXPECT_EQ("custom", folly::fmt_sized_format("{name}", named));
  EXPECT_EQ("custom", folly::fmt_sized_format(fmt::runtime("{name}"), named));
  EXPECT_EQ(
      "custom", folly::fmt_sized_format(FOLLY_FMT_COMPILE("{name}"), named));
}

TEST_F(FmtUtilityTest, fmt_sized_format_views) {
  std::array<int, 3> const values{1, 2, 3};
  auto const view = fmt::join(values, ", ");
  EXPECT_EQ("1, 2, 3", folly::fmt_sized_format("{}", view));
  EXPECT_EQ("1, 2, 3", folly::fmt_sized_format(fmt::runtime("{}"), view));
  EXPECT_EQ("1, 2, 3", folly::fmt_sized_format(FOLLY_FMT_COMPILE("{}"), view));
  EXPECT_EQ("1, 2, 3", folly::fmt_sized_format("{}", fmt::join(values, ", ")));
}

TEST_F(FmtUtilityTest, fmt_sized_format_preserves_arguments) {
  std::string value(128, 'x');
  auto const expected = value;
  EXPECT_EQ(expected, folly::fmt_sized_format("{}", value));
  EXPECT_EQ(expected, value);
  EXPECT_EQ(
      expected, folly::fmt_sized_format("{}", std::move(value))); // NOLINT
  // NOLINTNEXTLINE
  EXPECT_EQ(expected, value);
  EXPECT_EQ(
      expected,
      folly::fmt_sized_format(
          FOLLY_FMT_COMPILE("{}"), std::move(value))); // NOLINT
  // NOLINTNEXTLINE
  EXPECT_EQ(expected, value);
}

TEST_F(FmtUtilityTest, fmt_sized_format_size_mismatch) {
  std::string const large(4096, 'x');
  for (auto const first : {""sv, "a"sv, "long"sv, std::string_view(large)}) {
    for (auto const second : {""sv, "a"sv, "long"sv, std::string_view(large)}) {
      ChangingSize value{first, second};
      EXPECT_EQ(second, folly::fmt_sized_format("{}", value));
      EXPECT_EQ(2, value.calls);
      value.calls = 0;
      EXPECT_EQ(
          second, folly::fmt_sized_format(FOLLY_FMT_COMPILE("{}"), value));
      EXPECT_EQ(2, value.calls);
      value.calls = 0;
      EXPECT_EQ(second, folly::fmt_sized_format(fmt::runtime("{}"), value));
      EXPECT_EQ(2, value.calls);
    }
  }
}

TEST_F(FmtUtilityTest, fmt_make_format_args_from_map_fn) {
  EXPECT_EQ(
      "hello bob you silly goose",
      fmt::vformat(
          "hello {name} you {adj} goose",
          folly::fmt_make_format_args_from_map(
              std::map<std::string, std::string_view>{
                  {"name"s, "bob"sv},
                  {"adj"s, "silly"sv},
              })));
}

TEST_F(FmtUtilityTest, fmt_vformat_mangle) {
  EXPECT_EQ(
      "hello bob you silly goose",
      fmt::vformat(
          folly::fmt_vformat_mangle_format_string(
              "hello {@pre-key|name} you {@pre-key|adj} goose"),
          folly::fmt_make_format_args_from_map(
              std::map<std::string, std::string_view>{
                  {folly::fmt_vformat_mangle_name("@pre-key|name"), "bob"sv},
                  {folly::fmt_vformat_mangle_name("@pre-key|adj"), "silly"sv},
              })));
}

TEST_F(FmtUtilityTest, fmt_vformat_mangle_numeric_args_as_named) {
  auto const opts =
      folly::fmt_vformat_mangle_format_string_options{}
          .set_numeric_args_as_named(true);
  EXPECT_EQ(
      "hello bob you silly goose",
      fmt::vformat(
          folly::fmt_vformat_mangle_format_string(
              opts, "hello {123} you {456} goose"),
          folly::fmt_make_format_args_from_map(
              std::map<std::string, std::string_view>{
                  {folly::fmt_vformat_mangle_name("123"), "bob"sv},
                  {folly::fmt_vformat_mangle_name("456"), "silly"sv},
              })));
}
