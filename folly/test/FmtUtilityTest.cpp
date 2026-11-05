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

#include <folly/FBString.h>
#include <folly/FmtUtility.h>
#include <folly/Range.h>
#include <folly/lang/cstring_view.h>

#include <array>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/ranges.h>
#include <gtest/gtest.h>

using namespace std::literals;

namespace {
struct BulkOutput {
  std::string value;
  size_t chars = 0;
  size_t ranges = 0;

  void push_back(char c) {
    ++chars;
    value.push_back(c);
  }
  void append(std::string_view str) {
    ++ranges;
    value.append(str);
  }
};

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

struct SiblingString {
  bool named;
};

struct GenericText {
  std::string_view value;
  unsigned& counted;
  unsigned& written;
};

struct ParseConsumesIndex {};

struct BufferedOnlyText {};

struct ViewOnlyText {
  std::string_view value;
  constexpr operator std::string_view() const noexcept { return value; }
};

struct DecoratedStringLike {
  std::string_view value;
  mutable unsigned calls = 0;
  constexpr operator std::string_view() const noexcept { return value; }
};

struct ConversionOnlyText {
  std::string_view value;
  constexpr operator std::string_view() const noexcept { return value; }
};

struct InheritedDecoratedString : DecoratedStringLike {};

struct NativeMappedString : std::string {
  using std::string::string;
  mutable unsigned calls = 0;
};

struct ContextAwareSizedText {};

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

template <>
struct fmt::formatter<SiblingString> {
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }

  template <typename Context>
  auto format(SiblingString const& value, Context& ctx) const {
    auto const read = [](auto const& arg) -> fmt::string_view {
      using arg_type = std::remove_cvref_t<decltype(arg)>;
      constexpr bool is_string = std::is_same_v<arg_type, fmt::string_view>;
      constexpr bool is_missing = std::is_same_v<arg_type, fmt::monostate>;
      if constexpr (is_string) {
        return arg;
      } else if constexpr (is_missing) {
        return "<missing>";
      } else {
        throw std::invalid_argument("sibling is not a native string argument");
      }
    };
    int index = 0;
    auto const arg = value.named ? ctx.arg("source") : ctx.arg(index);
#if FMT_VERSION >= 110000
    auto const source = arg.visit(read);
#else
    auto const source = fmt::visit_format_arg(read, arg);
#endif
    return fmt::format_to(ctx.out(), "{}", source);
  }
};

template <>
struct fmt::formatter<ContextAwareSizedText>
    : folly::fmt_sized_formatter_base<fmt::formatter<ContextAwareSizedText>> {
  using folly::fmt_sized_formatter_base<
      fmt::formatter<ContextAwareSizedText>>::format;
  static size_t size_hint(ContextAwareSizedText const&) { return 0; }
  template <typename Sink>
  static void append(ContextAwareSizedText const&, Sink& sink) {
    sink.append("opaque");
  }
  template <typename Context>
  auto format(ContextAwareSizedText const&, Context& ctx) const {
    return fmt::formatter<SiblingString>{}.format(SiblingString{false}, ctx);
  }
};

template <>
struct fmt::formatter<ViewOnlyText> : formatter<fmt::string_view> {
  template <typename Context>
  auto format(ViewOnlyText const& value, Context& ctx) const {
    return formatter<fmt::string_view>::format(
        {value.value.data(), value.value.size()}, ctx);
  }
};

template <>
struct fmt::formatter<DecoratedStringLike> : formatter<fmt::string_view> {
  template <typename Context>
  auto format(DecoratedStringLike const& value, Context& ctx) const {
    ++value.calls;
    return fmt::format_to(ctx.out(), FOLLY_FMT_COMPILE("[{}]"), value.value);
  }
};

template <>
struct fmt::formatter<InheritedDecoratedString> : formatter<folly::fbstring> {
  template <typename Context>
  auto format(InheritedDecoratedString const& value, Context& ctx) const {
    return formatter<DecoratedStringLike>{}.format(value, ctx);
  }
};

template <>
struct fmt::formatter<NativeMappedString> : formatter<fmt::string_view> {
  template <typename Context>
  auto format(NativeMappedString const& value, Context& ctx) const {
    ++value.calls;
    return fmt::format_to(
        ctx.out(), FOLLY_FMT_COMPILE("[{}]"), std::string_view(value));
  }
};

template <>
struct fmt::formatter<GenericText> : formatter<fmt::string_view> {
  template <typename Context>
  auto format(GenericText const& value, Context& ctx) const {
    if constexpr (folly::detail::fmt_counting_format_context<Context>) {
      ++value.counted;
    } else {
      ++value.written;
    }
    return formatter<fmt::string_view>::format(
        {value.value.data(), value.value.size()}, ctx);
  }
};

template <>
struct fmt::formatter<BufferedOnlyText> {
  constexpr auto parse(fmt::format_parse_context& ctx) { return ctx.begin(); }
  auto format(BufferedOnlyText const&, fmt::format_context& ctx) const {
    return ctx.out();
  }
};

template <>
struct fmt::formatter<ParseConsumesIndex> {
  constexpr auto parse(fmt::format_parse_context& ctx) {
    ctx.next_arg_id();
    return ctx.begin();
  }
  template <typename Context>
  auto format(ParseConsumesIndex const&, Context& ctx) const {
    return fmt::format_to(ctx.out(), FOLLY_FMT_COMPILE("parse"));
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
  EXPECT_EQ(2, sized);
  EXPECT_EQ(2, appended);
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

TEST_F(FmtUtilityTest, fmt_sized_format_string_specs) {
  std::array const values{
      ""s,
      "abcdef"s,
      "a\0b"s,
      "\xc3\xa9\xe7\x95\x8c"s,
      std::string(1024, 'x'),
  };
  constexpr std::array specs{
      "[{}]",
      "[{0:}]",
      "[{0:>12}]",
      "[{0:*^12}]",
      "[{0:.1}]",
      "[{0:.2}]",
      "[{0:{1}.{2}}]",
      "{0} {0}",
  };
  for (auto const& value : values) {
    for (auto const spec : specs) {
      EXPECT_EQ(
          fmt::format(fmt::runtime(spec), value, 12, 2),
          folly::fmt_sized_format(fmt::runtime(spec), value, 12, 2));
      std::string_view const view(value);
      EXPECT_EQ(
          fmt::format(fmt::runtime(spec), view, 12, 2),
          folly::fmt_sized_format(fmt::runtime(spec), view, 12, 2));
    }
    EXPECT_EQ(
        fmt::format("[{}] [{:>{}.{}}]", value, value, 12, 2),
        folly::fmt_sized_format("[{}] [{:>{}.{}}]", value, value, 12, 2));
    EXPECT_EQ(
        fmt::format(FOLLY_FMT_COMPILE("[{}] [{:>{}.{}}]"), value, value, 12, 2),
        folly::fmt_sized_format(
            FOLLY_FMT_COMPILE("[{}] [{:>{}.{}}]"), value, value, 12, 2));
    std::string_view const view(value);
    EXPECT_EQ(
        fmt::format("[{}] [{:>{}.{}}]", view, view, 12, 1),
        folly::fmt_sized_format("[{}] [{:>{}.{}}]", view, view, 12, 1));
    EXPECT_EQ(
        fmt::format(FOLLY_FMT_COMPILE("[{}] [{:>{}.{}}]"), view, view, 12, 1),
        folly::fmt_sized_format(
            FOLLY_FMT_COMPILE("[{}] [{:>{}.{}}]"), view, view, 12, 1));
  }
  std::string const value = "abc";
  EXPECT_THROW(
      folly::fmt_sized_format(fmt::runtime("{:q}"), value), fmt::format_error);
}

TEST_F(FmtUtilityTest, fmt_sized_format_string_like_types) {
  std::array const values{
      ""s, "abc"s, "\xc3\xa9\xe7\x95\x8c"s, std::string(1024, 'x')};
  constexpr std::array specs{"[{}]", "[{:>12}]", "[{:.1}]", "[{:>{}.{}}]"};
  auto const check = [&](auto const& value) {
    for (auto const spec : specs) {
      EXPECT_EQ(
          fmt::format(fmt::runtime(spec), value, 12, 1),
          folly::fmt_sized_format(fmt::runtime(spec), value, 12, 1));
    }
    EXPECT_EQ(
        fmt::format("[{}][{:>12.1}]", value, value),
        folly::fmt_sized_format("[{}][{:>12.1}]", value, value));
    EXPECT_EQ(
        fmt::format(FOLLY_FMT_COMPILE("[{}][{:>12.1}]"), value, value),
        folly::fmt_sized_format(
            FOLLY_FMT_COMPILE("[{}][{:>12.1}]"), value, value));
    EXPECT_EQ(
        fmt::format("[{text}]", fmt::arg("text", value)),
        folly::fmt_sized_format("[{text}]", fmt::arg("text", value)));
  };
  for (auto const& value : values) {
    check(folly::fbstring(value.data(), value.size()));
    check(folly::cstring_view(value));
    check(folly::StringPiece(value));
    check(ViewOnlyText{value});
  }
  std::string const binary = "a\0b"s;
  check(folly::fbstring(binary.data(), binary.size()));
  check(folly::StringPiece(binary));
  check(ViewOnlyText{binary});
}

TEST_F(FmtUtilityTest, fmt_sized_format_decorated_string_like) {
  auto const check = []<typename T>(T const& value) {
    EXPECT_EQ("[text]", folly::fmt_sized_format("{}", value));
    EXPECT_EQ(2, value.calls);
    value.calls = 0;
    EXPECT_EQ("[text]", folly::fmt_sized_format(fmt::runtime("{}"), value));
    EXPECT_EQ(2, value.calls);
    value.calls = 0;
    EXPECT_EQ(
        "[text]", folly::fmt_sized_format(FOLLY_FMT_COMPILE("{}"), value));
    EXPECT_EQ(2, value.calls);
  };
  check(DecoratedStringLike{"text"});
  check(InheritedDecoratedString{{"text"}});
}

TEST_F(FmtUtilityTest, fmt_sized_format_conversion_only_string) {
  ConversionOnlyText const value{"text"};
  auto const view = std::string_view(value);
  EXPECT_EQ("[text]", folly::fmt_sized_format("[{}]", view));
  EXPECT_EQ("[text]", folly::fmt_sized_format(fmt::runtime("[{}]"), view));
  EXPECT_EQ("[text]", folly::fmt_sized_format(FOLLY_FMT_COMPILE("[{}]"), view));

  auto const check = []<typename T>(T const& arg) {
    if constexpr (fmt::is_formattable<T>::value) {
      EXPECT_EQ("[text]", folly::fmt_sized_format("[{}]", arg));
      EXPECT_EQ("[text]", folly::fmt_sized_format(fmt::runtime("[{}]"), arg));
      EXPECT_EQ(
          fmt::format("[{:>12.1}]", arg),
          folly::fmt_sized_format("[{:>12.1}]", arg));
    } else {
      EXPECT_FALSE((fmt::is_formattable<T>::value));
    }
  };
  check(value);
}

TEST_F(FmtUtilityTest, fmt_sized_format_native_string_mapping) {
  NativeMappedString const value{"text"};
  constexpr std::array specs{"{}", "[{:>8.1}]"};
  for (auto const spec : specs) {
    value.calls = 0;
    auto const size = fmt::formatted_size(fmt::runtime(spec), value);
    auto const expected = fmt::format(fmt::runtime(spec), value);
    auto const calls = value.calls;
    EXPECT_EQ(size, expected.size());
    value.calls = 0;
    EXPECT_EQ(expected, folly::fmt_sized_format(fmt::runtime(spec), value));
    EXPECT_EQ(calls, value.calls);
  }
}

TEST_F(FmtUtilityTest, fmt_sized_format_mixed_string_projection) {
  CustomValue const custom("custom");
  std::string const text = "text";
  std::array<int, 3> const numbers{1, 2, 3};
  EXPECT_EQ(
      "text custom 1,2,3",
      folly::fmt_sized_format(
          "{} {} {}", text, custom, fmt::join(numbers, ",")));
  EXPECT_EQ(
      "text custom 1,2,3",
      folly::fmt_sized_format(
          fmt::runtime("{} {} {}"), text, custom, fmt::join(numbers, ",")));
  EXPECT_EQ(
      "text custom 1,2,3",
      folly::fmt_sized_format(
          FOLLY_FMT_COMPILE("{} {} {}"),
          text,
          custom,
          fmt::join(numbers, ",")));
}

TEST_F(FmtUtilityTest, fmt_sized_format_named_strings) {
  using namespace fmt::literals;
  std::string const value = "a\0b"s;
  auto const expected = "[a\0b] 001f"s;
  EXPECT_EQ(
      expected,
      folly::fmt_sized_format(
          "[{text}] {number:04x}",
          fmt::arg("text", value),
          fmt::arg("number", 31)));
  EXPECT_EQ(
      expected,
      folly::fmt_sized_format(
          FOLLY_FMT_COMPILE("[{0}] {1:04x}"),
          "text"_a = value,
          "number"_a = 31));
  EXPECT_EQ(
      fmt::format(
          fmt::runtime("{text:>{width}.{precision}}"),
          fmt::arg("text", value),
          fmt::arg("width", 10),
          fmt::arg("precision", 2)),
      folly::fmt_sized_format(
          fmt::runtime("{text:>{width}.{precision}}"),
          fmt::arg("text", value),
          fmt::arg("width", 10),
          fmt::arg("precision", 2)));
}

TEST_F(FmtUtilityTest, fmt_sized_format_preserves_context_arguments) {
  for (auto const& value : {"a\0b"s, std::string(4096, 'x')}) {
    EXPECT_EQ(
        value, folly::fmt_sized_format("{1:}", value, SiblingString{false}));
    EXPECT_EQ(
        value,
        folly::fmt_sized_format(
            fmt::runtime("{1:}"), value, SiblingString{false}));
    EXPECT_EQ(
        value,
        folly::fmt_sized_format(
            FOLLY_FMT_COMPILE("{1:}"), value, SiblingString{false}));
    EXPECT_EQ(
        value, folly::fmt_sized_format("{1:}", value, ContextAwareSizedText{}));
    EXPECT_EQ(
        value,
        folly::fmt_sized_format(
            FOLLY_FMT_COMPILE("{1:}"), value, ContextAwareSizedText{}));
    SiblingString const readers[] = {{false}};
    EXPECT_EQ(
        fmt::format("{1}", value, readers),
        folly::fmt_sized_format("{1}", value, readers));
    EXPECT_EQ(
        fmt::format(fmt::runtime("{1}"), value, readers),
        folly::fmt_sized_format(fmt::runtime("{1}"), value, readers));
    EXPECT_EQ(
        value,
        folly::fmt_sized_format(
            "{reader:}",
            fmt::arg("source", value),
            fmt::arg("reader", SiblingString{true})));
    EXPECT_EQ(
        value,
        folly::fmt_sized_format(
            fmt::runtime("{reader:}"),
            fmt::arg("source", value),
            fmt::arg("reader", SiblingString{true})));
    EXPECT_EQ(
        fmt::format(
            FOLLY_FMT_COMPILE("{1:}"),
            fmt::arg("source", value),
            fmt::arg("reader", SiblingString{true})),
        folly::fmt_sized_format(
            FOLLY_FMT_COMPILE("{1:}"),
            fmt::arg("source", value),
            fmt::arg("reader", SiblingString{true})));
    auto const expected = fmt::format(
        "{reader}",
        fmt::arg("source", value),
        fmt::arg("reader", SiblingString{true}));
    EXPECT_EQ(
        expected,
        folly::fmt_sized_format(
            "{reader}",
            fmt::arg("source", value),
            fmt::arg("reader", SiblingString{true})));
    EXPECT_EQ(
        expected,
        folly::fmt_sized_format(
            fmt::runtime("{reader}"),
            fmt::arg("source", value),
            fmt::arg("reader", SiblingString{true})));
  }
}

TEST_F(FmtUtilityTest, fmt_sized_format_generic_context) {
  unsigned counted = 0;
  unsigned written = 0;
  std::string const text(1024, 'x');
  GenericText const value{text, counted, written};
  EXPECT_EQ(2049, folly::detail::fmt_generic_formatted_size("{0} {0}", value));
  EXPECT_EQ(2, counted);
  EXPECT_EQ(0, written);
  counted = written = 0;
  EXPECT_EQ(text + " " + text, folly::fmt_sized_format("{0} {0}", value));
  EXPECT_EQ(2, counted);
  EXPECT_EQ(2, written);
  counted = written = 0;
  EXPECT_EQ(
      fmt::format("{0:>{1}.{2}} {1}", value, 8, 3),
      folly::fmt_sized_format("{0:>{1}.{2}} {1}", value, 8, 3));
  EXPECT_EQ(1, counted);
  EXPECT_EQ(2, written);

  auto const supports_count_context = []<typename T> {
    return requires(
        fmt::formatter<T> const& formatter,
        T const& arg,
        folly::detail::fmt_counting_context& ctx) {
      formatter.format(arg, ctx);
    };
  };
  static_assert(supports_count_context.template operator()<GenericText>());
  static_assert(
      !supports_count_context.template operator()<BufferedOnlyText>());
}

TEST_F(FmtUtilityTest, fmt_sized_format_generic_parser) {
  EXPECT_EQ(
      "parse 7",
      folly::fmt_sized_format(fmt::runtime("{} {}"), ParseConsumesIndex{}, 7));
  EXPECT_EQ(
      7,
      folly::detail::fmt_generic_formatted_size(
          "{} {}", ParseConsumesIndex{}, 7));
  EXPECT_EQ(
      "parse 7",
      folly::fmt_sized_format(
          fmt::runtime("{} {number}"),
          ParseConsumesIndex{},
          fmt::arg("number", 7)));
  std::string const text = "abcdef";
  EXPECT_EQ(
      "     abc 7",
      folly::fmt_sized_format(fmt::runtime("{:>{}.{}} {}"), text, 8, 3, 7));
  constexpr std::array specs{
      "{} {0}",
      "{0} {}",
      "{3}",
      "{missing}",
      "{:q}",
      "{0:{1}}",
      "{0:.{1}}",
      "{0:{2}}"};
  for (auto const spec : specs) {
    EXPECT_THROW(
        (void)fmt::format(fmt::runtime(spec), text, -1), fmt::format_error);
    EXPECT_THROW(
        folly::fmt_sized_format(fmt::runtime(spec), text, -1),
        fmt::format_error);
  }
  int const number = 42;
  auto const pointer = static_cast<void const*>(&number);
  EXPECT_EQ(
      fmt::format("{:p}", pointer), folly::fmt_sized_format("{:p}", pointer));
}

TEST_F(FmtUtilityTest, fmt_sized_format_generic_containers) {
  std::vector<char> const vector{'a', 'b'};
  std::array<char, 2> const array{'a', 'b'};
  std::span<char const> const span(array);
  EXPECT_EQ(fmt::format("{}", vector), folly::fmt_sized_format("{}", vector));
  EXPECT_EQ(fmt::format("{}", array), folly::fmt_sized_format("{}", array));
  EXPECT_EQ(fmt::format("{}", span), folly::fmt_sized_format("{}", span));
  EXPECT_EQ(4, folly::detail::fmt_generic_formatted_size("{}", "text"));
}

TEST_F(FmtUtilityTest, fmt_sized_format_generic_native_specs) {
  auto const check = [](auto const& value, auto const& specs) {
    for (auto const spec : specs) {
      size_t size = 0;
      bool valid = true;
      try {
        size = fmt::formatted_size(fmt::runtime(spec), value, 12, 3);
      } catch (fmt::format_error const&) {
        valid = false;
      }
      if (valid) {
        EXPECT_EQ(
            size,
            folly::detail::fmt_generic_formatted_size(spec, value, 12, 3));
      } else {
        EXPECT_THROW(
            folly::detail::fmt_generic_formatted_size(spec, value, 12, 3),
            fmt::format_error);
      }
    }
  };
  constexpr std::array integer_specs{
      "{0}",
      "{0:08x}",
      "{0:#08X}",
      "{0:#016b}",
      "{0:#o}",
      "{0:+012d}",
      "{0:*>12}",
      "{0:^{1}}",
      "{0:L}",
  };
  check(-42, integer_specs);
  check(0u, integer_specs);
  check(123456789u, integer_specs);
  constexpr std::array float_specs{
      "{0}",
      "{0:12.3f}",
      "{0:+012.3e}",
      "{0:#g}",
      "{0:.3a}",
      "{0:{1}.{2}f}",
      "{0:.{2}g}",
  };
  check(1.23456789, float_specs);
  check(-0.0, float_specs);
  constexpr std::array string_specs{
      "{0}",
      "{0:>12}",
      "{0:^12.1}",
      "{0:.{2}}",
      "{0:{1}.{2}}",
      "{0:?}",
  };
  check("a\0b"sv, string_specs);
  check("\xc3\xa9\xe7\x95\x8c"sv, string_specs);
}

TEST_F(FmtUtilityTest, fmt_sized_format_generic_pointer) {
  int const number = 42;
  constexpr void const* null = nullptr;
  auto const check = [](auto pointer) {
    EXPECT_EQ(
        fmt::format("{:p}", pointer), folly::fmt_sized_format("{:p}", pointer));
    EXPECT_EQ(
        fmt::format(fmt::runtime("{:>24p}"), pointer),
        folly::fmt_sized_format(fmt::runtime("{:>24p}"), pointer));
    EXPECT_EQ(
        fmt::format(FOLLY_FMT_COMPILE("{:p}"), pointer),
        folly::fmt_sized_format(FOLLY_FMT_COMPILE("{:p}"), pointer));
  };
  check(static_cast<void const*>(&number));
  check(null);
  check("text");
}

TEST_F(FmtUtilityTest, fmt_sized_format_bulk_copy_dispatch) {
  std::string const value(1024, 'x');
  BulkOutput compiled;
  folly::detail::fmt_format_to(
      compiled, FOLLY_FMT_COMPILE("{}{}"), value, value);
  EXPECT_EQ(value + value, compiled.value);
  EXPECT_EQ(0, compiled.chars);
  EXPECT_GT(compiled.ranges, 0);

  BulkOutput runtime;
  folly::detail::fmt_format_to(runtime, fmt::runtime("{}{}"), value, value);
  EXPECT_EQ(value + value, runtime.value);
  EXPECT_EQ(0, runtime.chars);
  EXPECT_GT(runtime.ranges, 0);

  BulkOutput checked;
  folly::detail::fmt_format_to(checked, "{}{}", value, value);
  EXPECT_EQ(value + value, checked.value);
  EXPECT_EQ(0, checked.chars);
  EXPECT_GT(checked.ranges, 0);
}

TEST_F(FmtUtilityTest, fmt_sized_format_buffer_flush_boundaries) {
  static_assert(!std::is_move_constructible_v<
                folly::detail::fmt_output_buffer<BulkOutput>>);
  constexpr std::array sizes{0, 1, 255, 256, 257, 511, 512, 513, 4096};
  for (auto const size : sizes) {
    std::string const value(size, 'x');
    BulkOutput out;
    folly::detail::fmt_output_buffer buffer(out);
    fmt::format_to(fmt::appender(buffer), fmt::runtime("{}"), value);
    buffer.flush();
    EXPECT_EQ(value, out.value);
    EXPECT_EQ(0, out.chars);
    EXPECT_EQ(size != 0, out.ranges != 0);
    auto const ranges = out.ranges;
    buffer.flush();
    EXPECT_EQ(value, out.value);
    EXPECT_EQ(ranges, out.ranges);
  }
}

TEST_F(FmtUtilityTest, fmt_sized_format_output_sink) {
  auto const spec = "{:+018d}|{:#010x}|{:8.3f}|{:>5}";
  auto const expected =
      fmt::format(fmt::runtime(spec), -123, 0x12ab, 1.25, true);
  EXPECT_EQ(
      expected,
      folly::fmt_sized_format(fmt::runtime(spec), -123, 0x12ab, 1.25, true));
  EXPECT_EQ(
      expected,
      folly::fmt_sized_format(
          "{:+018d}|{:#010x}|{:8.3f}|{:>5}", -123, 0x12ab, 1.25, true));
  EXPECT_EQ(
      expected,
      folly::fmt_sized_format(
          FOLLY_FMT_COMPILE("{:+018d}|{:#010x}|{:8.3f}|{:>5}"),
          -123,
          0x12ab,
          1.25,
          true));
  std::string_view const empty;
  EXPECT_EQ(
      "x", folly::fmt_sized_format(FOLLY_FMT_COMPILE("{}x{}"), empty, empty));
  EXPECT_EQ("x", folly::fmt_sized_format("{}x{}", empty, empty));
  std::string const large(4096, 'x');
  EXPECT_EQ(large + large, folly::fmt_sized_format("{}{}", large, large));
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

TEST_F(FmtUtilityTest, fmt_vformat_mangle_name_bounded_view) {
  auto const backing = "name\0suffix"s;
  auto const key = std::string_view(backing.data(), 4);
  EXPECT_EQ(
      folly::fmt_vformat_mangle_name("name"),
      folly::fmt_vformat_mangle_name(key));
  EXPECT_NE(
      folly::fmt_vformat_mangle_name(backing),
      folly::fmt_vformat_mangle_name(key));
  std::string out = "prefix";
  folly::fmt_vformat_mangle_name(out, key);
  EXPECT_EQ("prefix" + folly::fmt_vformat_mangle_name("name"), out);
}

TEST_F(FmtUtilityTest, fmt_vformat_mangle_name_binary_view) {
  std::array<char, 4> const bytes{'a', '\0', char(0xff), 'b'};
  std::string const owned(bytes.data(), bytes.size());
  auto const key = std::string_view(bytes.data(), bytes.size());
  EXPECT_EQ(
      folly::fmt_vformat_mangle_name(owned),
      folly::fmt_vformat_mangle_name(key));
  EXPECT_NE(
      folly::fmt_vformat_mangle_name("a"), folly::fmt_vformat_mangle_name(key));
  EXPECT_EQ(
      folly::fmt_vformat_mangle_name(""),
      folly::fmt_vformat_mangle_name(std::string_view{}));
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
