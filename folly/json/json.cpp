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

#include <folly/json/json.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <functional>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

#include <fmt/compile.h>
#include <glog/logging.h>

#include <folly/CPortability.h>
#include <folly/Conv.h>
#include <folly/Likely.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/Unicode.h>
#include <folly/Utility.h>
#include <folly/lang/Bits.h>

namespace folly {

//////////////////////////////////////////////////////////////////////

namespace json {

namespace {

using std::string_view_literals::operator""sv;

parse_error make_parse_error(
    unsigned int line,
    std::string const& context,
    std::string const& expected) {
  return parse_error(
      to<std::string>(
          "json parse error on line ",
          line,
          !context.empty() ? to<std::string>(" near `", context, '\'') : "",
          ": ",
          expected));
}

bool allowTrailingComma(json::serialization_opts const& opts) {
  return opts.allow_trailing_comma || opts.allow_json5_experimental;
}

bool allowNanInf(json::serialization_opts const& opts) {
  return opts.allow_nan_inf || opts.allow_json5_experimental;
}

// JSON5 uses ECMAScript 5.1 IdentifierName for unquoted keys.
// For ASCII, this means: start with [a-zA-Z_$], continue with [a-zA-Z0-9_$].
bool isIdentifierStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
      c == '$';
}

bool isIdentifierPart(char c) {
  return isIdentifierStart(c) || (c >= '0' && c <= '9');
}

constexpr uint8_t hexDecodeDigitRaw(char c) {
  // TODO: switch to folly::hex_decode_digit_raw(c)
  if ('0' <= c && c <= '9') {
    return c - '0';
  }
  if ('A' <= c && c <= 'F') {
    return c - 'A' + 10;
  }
  if ('a' <= c && c <= 'f') {
    return c - 'a' + 10;
  }
  folly::assume_unreachable();
}

std::optional<dynamic> parseHexInteger(
    StringPiece str, bool isNegative, bool doubleFallback) {
  uint64_t uval = 0;
  auto [ptr, ec] =
      std::from_chars(str.data(), str.data() + str.size(), uval, 16);

  if (ec != std::errc::result_out_of_range) {
    constexpr auto kMinAsUnsigned =
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
    if (uval == kMinAsUnsigned && isNegative) {
      return std::numeric_limits<int64_t>::min();
    }
    if (uval <= std::numeric_limits<int64_t>::max()) {
      return isNegative
          ? -static_cast<int64_t>(uval)
          : static_cast<int64_t>(uval);
    }
  }

  if (doubleFallback) {
    // std::from_chars for floating-point with hex format is not available
    // on all platforms (iOS < 26.0, Android), so use handwritten loop instead.
    double dval = 0;
    for (char c : str) {
      dval = dval * 16 + hexDecodeDigitRaw(c);
    }
    if (std::isfinite(dval)) {
      return isNegative ? -dval : dval;
    }
  }

  return std::nullopt;
}

// Context class allows to restore the path to element that we are about to
// print so that if error happens we can throw meaningful exception.
class Context {
 public:
  Context(const Context* parent_context, const dynamic& key)
      : parent_context_(parent_context), key_(key), is_key_(false) {}
  Context(const Context* parent_context, const dynamic& key, bool is_key)
      : parent_context_(parent_context), key_(key), is_key_(is_key) {}

  // Return location description of a context as a chain of keys
  // ex., '"outherKey"->"innerKey"'.
  std::string locationDescription() const;
  std::string getName() const;
  std::string typeDescription() const { return is_key_ ? "key" : "value"; }

 private:
  const Context* const parent_context_;
  const dynamic& key_;
  bool is_key_;
};

// Forward declaration; defined after PrinterImpl below.
std::string toStringOr(dynamic const& v, const char* placeholder);

std::string Context::locationDescription() const {
  std::vector<std::string> keys;
  const Context* ptr = parent_context_;
  while (ptr) {
    keys.push_back(ptr->getName());
    ptr = ptr->parent_context_;
  }
  keys.push_back(getName());
  std::ostringstream stream;
  std::reverse_copy(
      keys.begin(),
      keys.end() - 1,
      std::ostream_iterator<std::string>(stream, "->"));
  // Add current key.
  stream << keys.back();
  return stream.str();
}

std::string Context::getName() const {
  return toStringOr(key_, "<unprintable>");
}

std::string contextDescription(const Context* context) {
  if (!context) {
    return "<undefined location>";
  }
  return context->typeDescription() + " at " + context->locationDescription();
}

// Printer is templated on `Pretty` to let the compiler eliminate the
// `if (indentLevel_)` runtime branches in `newline()`, `mapColon()`,
// `indent()`, and `outdent()` for the common compact-output path.
template <bool Pretty>
struct PrinterImpl {
  explicit PrinterImpl(std::string& out, serialization_opts const* opts)
      : out_(out), opts_(*opts) {}

  void operator()(dynamic const& v, const Context& context) const {
    (*this)(v, &context);
  }
  void operator()(dynamic const& v, const Context* context) const {
    switch (v.type()) {
      case dynamic::DOUBLE: {
        const double dval = v.asDouble();
        if (!allowNanInf(opts_)) {
          if (std::isnan(dval)) {
            throw json::print_error(
                "folly::toJson: JSON object value was a NaN when serializing " +
                contextDescription(context));
          }
          if (std::isinf(dval)) {
            throw json::print_error(
                "folly::toJson: JSON object value was an INF when serializing " +
                contextDescription(context));
          }
        }
        appendDouble(dval);
        break;
      }
      case dynamic::INT64: {
        auto intval = v.asInt();
        if (opts_.javascript_safe) {
          // Use folly::to to check that this integer can be represented
          // as a double without loss of precision.
          intval = int64_t(to<double>(intval));
        }
        toAppend(intval, &out_);
        break;
      }
      case dynamic::BOOL:
        out_.append(v.asBool() ? "true"sv : "false"sv);
        break;
      case dynamic::NULLT:
        out_.append("null"sv);
        break;
      case dynamic::STRING:
        escapeString(v.stringPiece(), out_, opts_);
        break;
      case dynamic::OBJECT:
        printObject(v, context);
        break;
      case dynamic::ARRAY:
        printArray(v, context);
        break;
      default:
        CHECK(0) << "Bad type " << v.type();
    }
  }

 private:
  void printKV(
      dynamic const& o,
      const std::pair<const dynamic, dynamic>& p,
      const Context* context) const {
    if (opts_.convert_int_keys && p.first.isInt()) {
      auto strKey = p.first.asString();
      if (o.count(strKey)) {
        throw json::print_error(
            folly::to<std::string>(
                "folly::toJson: Source object has integer and string keys "
                "representing the same value: ",
                p.first.asInt()));
      }
      (*this)(p.first.asString(), Context(context, p.first, true));
    } else if (!opts_.allow_non_string_keys && !p.first.isString()) {
      throw json::print_error(
          "folly::toJson: JSON object key " +
          toStringOr(p.first, "<unprintable key>") + " was not a string " +
          (opts_.convert_int_keys ? "or integer " : "") +
          "when serializing key at " +
          Context(context, p.first, true).locationDescription());
    } else {
      (*this)(p.first, Context(context, p.first, true)); // Key
    }
    mapColon();
    (*this)(p.second, Context(context, p.first, false)); // Value
  }

  template <typename Iterator>
  void printKVPairs(
      dynamic const& o, Iterator begin, Iterator end, const Context* context)
      const {
    printKV(o, *begin, context);
    for (++begin; begin != end; ++begin) {
      out_.push_back(',');
      newline();
      printKV(o, *begin, context);
    }
  }

  void printObject(dynamic const& o, const Context* context) const {
    if (o.empty()) {
      out_.append("{}"sv);
      return;
    }

    out_.push_back('{');
    indent();
    newline();
    if (opts_.sort_keys || opts_.sort_keys_by) {
      using ref = std::reference_wrapper<decltype(o.items())::value_type const>;
      auto sort_keys_by = [&](auto begin, auto end, const auto& comp) {
        std::sort(begin, end, [&](ref a, ref b) {
          // Only compare keys.  No ordering among identical keys.
          return comp(a.get().first, b.get().first);
        });
      };
      std::vector<ref> refs(o.items().begin(), o.items().end());
      if (opts_.sort_keys_by) {
        sort_keys_by(refs.begin(), refs.end(), opts_.sort_keys_by);
      } else {
        sort_keys_by(refs.begin(), refs.end(), std::less<>());
      }
      printKVPairs(o, refs.cbegin(), refs.cend(), context);
    } else {
      printKVPairs(o, o.items().begin(), o.items().end(), context);
    }
    outdent();
    newline();
    out_.push_back('}');
  }

  void printArray(dynamic const& a, const Context* context) const {
    if (a.empty()) {
      out_.append("[]"sv);
      return;
    }

    out_.push_back('[');
    indent();
    newline();
    auto it = a.begin();
    auto end = a.end();
    (*this)(*it, Context(context, dynamic(int64_t{0})));
    ++it;
    for (int64_t i = 1; it != end; ++it, ++i) {
      out_.push_back(',');
      newline();
      (*this)(*it, Context(context, dynamic(i)));
    }
    outdent();
    newline();
    out_.push_back(']');
  }

  void outdent() const {
    if constexpr (Pretty) {
      --indentLevel_;
    }
  }
  void indent() const {
    if constexpr (Pretty) {
      ++indentLevel_;
    }
  }
  void newline() const {
    if constexpr (Pretty) {
      out_.push_back('\n');
      out_.append(
          static_cast<size_t>(indentLevel_) *
              opts_.pretty_formatting_indent_width,
          ' ');
    }
  }
  void mapColon() const { out_.append(Pretty ? ": "sv : ":"sv); }

  // Append `dval` to `out_` using fmt's Dragonbox. Infinity/NaN use the
  // JSON spellings ("Infinity"/"-Infinity"/"NaN") since fmt emits "inf"/"nan".
  void appendDouble(double dval) const {
    if (std::isinf(dval)) {
      out_.append(dval > 0 ? "Infinity"sv : "-Infinity"sv);
      return;
    }
    if (std::isnan(dval)) {
      out_.append("NaN"sv);
      return;
    }
    auto out = std::back_inserter(out_);
    switch (opts_.float_format) {
      case FloatFormat::SHORTEST:
        fmt::format_to(out, FMT_COMPILE("{}"), dval);
        return;
      case FloatFormat::SHORTEST_TRAILING_DOT_ZERO:
        appendShortestTrailingDotZero(dval);
        return;
      case FloatFormat::SHORTEST_SINGLE: {
        const float fval = static_cast<float>(dval);
        if (std::isinf(fval)) {
          out_.append(fval > 0 ? "Infinity"sv : "-Infinity"sv);
          return;
        }
        if (std::isnan(fval)) {
          out_.append("NaN"sv);
          return;
        }
        fmt::format_to(out, FMT_COMPILE("{}"), fval);
        return;
      }
      case FloatFormat::SHORTEST_SINGLE_TRAILING_DOT_ZERO: {
        const float fval = static_cast<float>(dval);
        if (std::isinf(fval)) {
          out_.append(fval > 0 ? "Infinity"sv : "-Infinity"sv);
          return;
        }
        if (std::isnan(fval)) {
          out_.append("NaN"sv);
          return;
        }
        appendShortestTrailingDotZero(fval);
        return;
      }
      case FloatFormat::FIXED:
        fmt::format_to(
            out, FMT_COMPILE("{:.{}f}"), dval, opts_.double_num_digits);
        return;
      case FloatFormat::GENERAL:
        fmt::format_to(
            out, FMT_COMPILE("{:.{}g}"), dval, opts_.double_num_digits);
        return;
    }
  }

  // Format `fval` (double or float) with shortest round-trip notation,
  // guaranteeing at least one digit after the decimal point as required by
  // RFC 8259 §6. fmt's {:#} alternate form satisfies this for values whose
  // shortest representation uses decimal notation, but emits a bare trailing
  // dot for integer-mantissa values in scientific notation (e.g. 3e-6 →
  // "3.e-06"), which is invalid JSON. We format with "{}" into a 32-byte
  // stack buffer, then emit the three segments around the injected ".0"
  // directly into out_, avoiding any memmove on the output string.
  template <typename Float>
  void appendShortestTrailingDotZero(Float fval) const {
    // longest shortest-round-trip double is ~24 chars
    std::array<char, 32> buf = {};
    const auto result =
        fmt::format_to_n(buf.data(), buf.size(), FMT_COMPILE("{}"), fval);
    const char* const p = buf.data();
    const char* const end = result.out;
    for (const char* c = p; c != end; ++c) {
      if (*c == '.') {
        out_.append(p, end); // already has a fractional digit — no fixup
        return;
      }
      if (*c == 'e' || *c == 'E') {
        out_.append(p, c); // mantissa
        out_.append(".0"); // injected fractional part
        out_.append(c, end); // exponent
        return;
      }
    }
    out_.append(p, end); // integer in decimal notation: "45" → "45.0"
    out_.append(".0");
  }

  std::string& out_;
  serialization_opts const& opts_;
  // Mutable because the const operator()/printX methods mutate it via
  // indent()/outdent(); only present (logically) when Pretty.
  mutable unsigned indentLevel_{0};
};

// Free helper used by Context::getName (and a few error paths) to print
// a single dynamic value as a short string. Uses the pretty-format
// instantiation to match the prior behavior — the legacy Printer was
// instantiated with a non-null indentLevel pointer here, which made
// composite keys (rare; only with allow_non_string_keys) render with
// newlines + indentation in error messages. For scalar keys (the
// common case) this is byte-identical to the compact instantiation.
std::string toStringOr(dynamic const& v, const char* placeholder) {
  try {
    std::string result;
    serialization_opts opts;
    opts.allow_nan_inf = true;
    opts.allow_non_string_keys = true;
    PrinterImpl<true> printer(result, &opts);
    printer(v, nullptr);
    return result;
  } catch (...) {
    return placeholder;
  }
}

//////////////////////////////////////////////////////////////////////

// Smart ptr that returns default constructed serialization_opts
struct DefaultOptsPtr {
  serialization_opts operator*() const { return {}; }
};

// Match the configured quote, backslashes, and controls (< 0x20) in a word.
// Pack input bytes least-significant first. Only the first marked byte is
// exact; later marks may be borrow artifacts.
struct StringBodyMatcher {
  static constexpr uint64_t kOnes = 0x0101010101010101ULL;
  static constexpr uint64_t kHighs = 0x8080808080808080ULL;

  explicit StringBodyMatcher(char quoteChar)
      : quoteBcast_(kOnes * static_cast<uint8_t>(quoteChar)) {}

  FOLLY_ALWAYS_INLINE uint64_t operator()(uint64_t word) const {
    const auto zeroByte = [](uint64_t v) { return (v - kOnes) & ~v & kHighs; };
    const auto controls = (word - kOnes * 0x20) & ~word & kHighs;
    return zeroByte(word ^ quoteBcast_) |
        zeroByte(word ^ (kOnes * static_cast<uint8_t>('\\'))) | controls;
  }

  uint64_t quoteBcast_;
};

// Wraps our input buffer with some helper functions.
template <class OptsPtr = serialization_opts const*>
struct Input {
  explicit Input(StringPiece range, OptsPtr opts)
      : range_(range), opts_(opts), lineNum_(0) {
    storeCurrent();
  }

  Input(Input const&) = delete;
  Input& operator=(Input const&) = delete;

  char const* begin() const { return range_.begin(); }

  unsigned getLineNum() const { return lineNum_; }

  // Parse ahead for as long as the supplied predicate is satisfied,
  // returning a range of what was skipped.
  template <class Predicate>
  StringPiece skipWhile(const Predicate& p) {
    std::size_t skipped = 0;
    for (; skipped < range_.size(); ++skipped) {
      if (!p(range_[skipped])) {
        break;
      }
      if (range_[skipped] == '\n') {
        ++lineNum_;
      }
    }
    auto ret = range_.subpiece(0, skipped);
    range_.advance(skipped);
    storeCurrent();
    return ret;
  }

  // Consume the initial unescaped run after the opening quote.
  // Return a borrowed span; leave quote/backslash or JSON5 CR/LF unconsumed.
  StringPiece skipStringBody(char quoteChar) {
    const char* const base = range_.data();
    const size_t n = range_.size();
    size_t i = 0;
    const StringBodyMatcher matchBody(quoteChar);
    while (n - i >= sizeof(uint64_t)) {
      const auto word = Endian::little(loadUnaligned<uint64_t>(base + i));
      const auto match = matchBody(word);
      if (!match) {
        i += sizeof(uint64_t);
        continue;
      }
      i += (findFirstSet(match) - 1) >> 3;
      const char c = base[i];
      if (LIKELY(c == quoteChar) || c == '\\') {
        goto bodyDone;
      }
      i = skipStringBodyWithControls(quoteChar, i);
      goto bodyDone;
    }
    for (; i < n; ++i) {
      const char c = base[i];
      if (c == quoteChar || c == '\\') {
        break;
      }
      if (static_cast<uint8_t>(c) < 0x20) {
        i = skipStringBodyWithControls(quoteChar, i);
        break;
      }
    }
  bodyDone:
    const auto ret = range_.subpiece(0, i);
    range_.advance(i);
    storeCurrent();
    return ret;
  }

  // Scan [i, end) within the current input, using absolute offsets.
  // Stop at quote/backslash or JSON5 CR/LF; return the stop index or end.
  // Count accepted LF, but do not advance the cursor.
  FOLLY_ALWAYS_INLINE size_t
  skipStringBodyScalar(char quoteChar, bool json5, size_t i, size_t end) {
    for (; i < end; ++i) {
      const char c = range_[i];
      if (c == quoteChar || c == '\\' || (json5 && (c == '\n' || c == '\r'))) {
        break;
      }
      if (c == '\n') {
        ++lineNum_;
      }
    }
    return i;
  }

  // Use an inline eight-byte probe, then the hybrid scanner for longer runs.
  // Same borrowed-span and cursor contract as skipStringBody().
  StringPiece skipStringBodyAfterEscape(char quoteChar) {
    const bool json5 = getOpts().allow_json5_experimental;
    const size_t scalarEnd = std::min(range_.size(), sizeof(uint64_t));
    size_t i = skipStringBodyScalar(quoteChar, json5, 0, scalarEnd);
    if (i < scalarEnd) {
      goto bodyDone;
    }
    if (i < range_.size()) {
      i = skipStringBodyWithControls(quoteChar, i);
    }
  bodyDone:
    const auto ret = range_.subpiece(0, i);
    range_.advance(i);
    storeCurrent();
    return ret;
  }

  // Hybrid scan from offset i in the current range; earlier bytes are
  // accounted. Count accepted LF and return the stop offset or range_.size().
  // Leave the cursor unchanged.
  FOLLY_NOINLINE size_t skipStringBodyWithControls(char quoteChar, size_t i) {
    const bool json5 = getOpts().allow_json5_experimental;
    const StringBodyMatcher matchBody(quoteChar);
    const size_t n = range_.size();
    while (i < n) {
      const size_t scalarEnd = i + std::min(n - i, sizeof(uint64_t));
      i = skipStringBodyScalar(quoteChar, json5, i, scalarEnd);
      if (i < scalarEnd) {
        return i;
      }
      while (n - i >= sizeof(uint64_t)) {
        const auto word =
            Endian::little(loadUnaligned<uint64_t>(range_.data() + i));
        const auto match = matchBody(word);
        if (match) {
          i += (findFirstSet(match) - 1) >> 3;
          break;
        }
        i += sizeof(uint64_t);
      }
    }
    return i;
  }

  StringPiece skipDigits() {
    return skipWhile([](char c) { return c >= '0' && c <= '9'; });
  }

  StringPiece skipSignAndDigits() {
    bool firstChar = true;
    return skipWhile([&firstChar](char c) {
      bool result =
          (c >= '0' && c <= '9') || (firstChar && (c == '-' || c == '+'));
      firstChar = false;
      return result;
    });
  }

  bool skipWhitespaceOnly() {
    unsigned index = 0;
    while (true) {
      while (index < range_.size() && range_[index] == ' ') {
        index++;
      }
      if (index < range_.size()) {
        if (range_[index] == '\n') {
          index++;
          ++lineNum_;
          continue;
        }
        if (range_[index] == '\t' || range_[index] == '\r') {
          index++;
          continue;
        }
        if (getOpts().allow_json5_experimental &&
            (range_[index] == '\f' || range_[index] == '\v')) {
          index++;
          continue;
        }
      }
      break;
    }
    range_.advance(index);
    storeCurrent();
    return index;
  }

  bool skipComment() {
    if (!getOpts().allow_json5_experimental) {
      return false;
    }
    if (consume("//")) {
      // Single-line comment: skip until CR or LF.
      char prev = 0;
      skipWhile([&prev](char curr) {
        if (prev == '\r' || prev == '\n') {
          return false;
        }
        prev = curr;
        return true;
      });
      return true;
    }
    if (consume("/*")) {
      // Block comment: skip until closing "*/".
      char prev = 0;
      skipWhile([&prev](char curr) {
        if (prev == '*' && curr == '/') {
          return false;
        }
        prev = curr;
        return true;
      });
      if (consume("/")) {
        return true;
      }
      error("unterminated block comment");
    }
    return false;
  }

  void skipWhitespace() {
    do {
      skipWhitespaceOnly();
      // Loop to handle adjacent comments, e.g. `/*a*//*b*/`.
    } while (skipComment());
  }

  void expect(char c) {
    if (**this != c) {
      throw json::make_parse_error(
          lineNum_, context(), to<std::string>("expected '", c, '\''));
    }
    ++*this;
  }

  std::size_t size() const { return range_.size(); }

  int operator*() const { return current_; }

  void operator++() {
    range_.pop_front();
    storeCurrent();
  }

  void advance(size_t n) {
    range_.advance(n);
    storeCurrent();
  }

  template <class T>
  T extract() {
    try {
      return to<T>(&range_);
    } catch (std::exception const& e) {
      error(e.what());
    }
  }

  bool consume(StringPiece str) {
    if (range_.startsWith(str)) {
      range_.advance(str.size());
      storeCurrent();
      return true;
    }
    return false;
  }

  std::string context() const {
    return range_.subpiece(0, 16 /* arbitrary */).toString();
  }

  [[noreturn]] void error(char const* what) const {
    throw json::make_parse_error(lineNum_, context(), what);
  }
  template <typename R>
  R error(char const* what) const {
    error(what);
  }

  decltype(auto) getOpts() { return *opts_; }

  void incrementRecursionLevel() {
    if (currentRecursionLevel_ > getOpts().recursion_limit) {
      error("recursion limit exceeded");
    }
    currentRecursionLevel_++;
  }

  void decrementRecursionLevel() { currentRecursionLevel_--; }

 private:
  void storeCurrent() { current_ = range_.empty() ? EOF : range_.front(); }

 private:
  StringPiece range_;
  [[FOLLY_ATTR_NO_UNIQUE_ADDRESS]] OptsPtr opts_;
  unsigned lineNum_;
  int current_;
  unsigned int currentRecursionLevel_{0};
};

template <class T>
class RecursionGuard {
 public:
  explicit RecursionGuard(Input<T>& in) : in_(in) {
    in_.incrementRecursionLevel();
  }

  ~RecursionGuard() { in_.decrementRecursionLevel(); }

 private:
  Input<T>& in_;
};

template <class T>
dynamic parseValue(Input<T>& in, json::metadata_map* map);
template <class T>
std::string parseString(Input<T>& in, char quoteChar = '"');
template <class T>
dynamic parseNumber(Input<T>& in);

template <class T>
void parseObjectKeyValue(
    Input<T>& in,
    dynamic& ret,
    dynamic&& key,
    json::metadata_map* map,
    bool distinct) {
  auto keyLineNumber = in.getLineNum();
  in.skipWhitespace();
  in.expect(':');
  in.skipWhitespace();
  auto valueLineNumber = in.getLineNum();
  auto value = parseValue(in, map);
  auto [it, inserted] = ret.try_emplace(std::move(key), std::move(value));
  if (!inserted) {
    if (distinct) {
      in.error("duplicate key inserted");
    }
    it->second = std::move(value);
  }
  if (map) {
    map->emplace(
        &it->second,
        json::parse_metadata{{{keyLineNumber}}, {{valueLineNumber}}});
  }
}

template <class T>
dynamic parseObject(Input<T>& in, json::metadata_map* map) {
  DCHECK_EQ(*in, '{');
  ++in;

  dynamic ret = dynamic::object;

  in.skipWhitespace();
  if (*in == '}') {
    ++in;
    return ret;
  }

  const auto& opts = in.getOpts();
  const bool distinct = opts.validate_keys || opts.convert_int_keys;
  for (;;) {
    if (allowTrailingComma(opts) && *in == '}') {
      break;
    }
    dynamic key = opts.allow_json5_experimental && isIdentifierStart(*in)
        ? in.skipWhile(isIdentifierPart)
        : parseValue(in, map);
    if (opts.convert_int_keys && key.isInt()) {
      key = key.asString();
    } else if (!opts.allow_non_string_keys && !key.isString()) {
      in.error(
          opts.convert_int_keys
              ? "expected string or integer for object key"
              : "expected string for object key");
    }
    parseObjectKeyValue(in, ret, std::move(key), map, distinct);

    in.skipWhitespace();
    if (*in != ',') {
      break;
    }
    ++in;
    in.skipWhitespace();
  }
  in.expect('}');

  return ret;
}

template <class T>
dynamic parseArray(Input<T>& in, json::metadata_map* map) {
  DCHECK_EQ(*in, '[');
  ++in;

  dynamic ret = dynamic::array;

  in.skipWhitespace();
  if (*in == ']') {
    ++in;
    return ret;
  }

  std::vector<uint32_t> lineNumbers;
  for (;;) {
    if (allowTrailingComma(in.getOpts()) && *in == ']') {
      break;
    }
    ret.push_back(parseValue(in, map));
    if (map) {
      lineNumbers.push_back(in.getLineNum());
    }
    in.skipWhitespace();
    if (*in != ',') {
      break;
    }
    ++in;
    in.skipWhitespace();
  }
  if (map) {
    for (size_t i = 0, e = ret.size(); i < e; i++) {
      map->emplace(&ret[i], json::parse_metadata{{{0}}, {{lineNumbers[i]}}});
    }
  }
  in.expect(']');

  return ret;
}

template <class T>
dynamic parseNumber(Input<T>& in) {
  bool const json5 = in.getOpts().allow_json5_experimental;
  bool const positive = *in == '+';
  if (positive && !json5) {
    in.error("expected json value");
  }
  if (positive && in.consume("+Infinity")) {
    if (in.getOpts().parse_numbers_as_strings) {
      return "+Infinity";
    }
    return std::numeric_limits<double>::infinity();
  }
  bool const negative = (*in == '-');
  if (negative && in.consume("-Infinity")) {
    if (in.getOpts().parse_numbers_as_strings) {
      return "-Infinity";
    } else {
      return -std::numeric_limits<double>::infinity();
    }
  }

  auto integral = in.skipSignAndDigits();
  if ((positive || negative) && integral.size() < 2 && (*in != '.' || !json5)) {
    in.error(
        positive ? "expected digits after `+`" : "expected digits after `-`");
  }

  const bool isIntegralZero =
      (integral == "0" || integral == "+0" || integral == "-0");

  if (json5 && isIntegralZero && (in.consume("x") || in.consume("X"))) {
    auto hexDigits = in.skipWhile([](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F');
    });
    if (hexDigits.empty()) {
      in.error("expected hex digits after '0x'");
    }
    if (in.getOpts().parse_numbers_as_strings) {
      return range(integral.begin(), hexDigits.end());
    }
    if (auto result = parseHexInteger(
            hexDigits, negative, in.getOpts().double_fallback)) {
      in.skipWhitespace();
      return *result;
    }
    in.error("hex literal out of range");
  }

  // JSON5 forbids leading zeros (octal/noctal literals like 010, 080, 00).
  if (json5 && !integral.empty() && !isIntegralZero) {
    auto digitStart = integral.begin() + (positive || negative);
    if (*digitStart == '0') {
      in.error("leading zeros in numbers are not allowed");
    }
  }

  auto const wasE = *in == 'e' || *in == 'E';

  if (*in != '.' && !wasE && in.getOpts().parse_numbers_as_strings) {
    return integral;
  }

  constexpr const char* maxIntStr = "9223372036854775807";
  constexpr const char* minIntStr = "-9223372036854775808";
  constexpr auto maxIntLen = std::char_traits<char>::length(maxIntStr);
  constexpr auto minIntLen = std::char_traits<char>::length(minIntStr);
  auto extremaLen = negative ? minIntLen : maxIntLen;
  auto extremaStr = negative ? minIntStr : maxIntStr;
  if (*in != '.' && !wasE) {
    if (FOLLY_LIKELY(
            !in.getOpts().double_fallback || integral.size() < extremaLen) ||
        (integral.size() == extremaLen && integral <= extremaStr)) {
      auto val = to<int64_t>(integral);
      in.skipWhitespace();
      return val;
    } else {
      auto val = to<double>(integral);
      in.skipWhitespace();
      return val;
    }
  }

  auto end = !wasE ? (++in, in.skipDigits().end()) : in.begin();
  if (*in == 'e' || *in == 'E') {
    ++in;
    if (*in == '+' || *in == '-') {
      ++in;
    }
    auto expPart = in.skipDigits();
    end = expPart.end();
  }
  auto fullNum = range(integral.begin(), end);
  if (in.getOpts().parse_numbers_as_strings) {
    return fullNum;
  }
  auto val = to<double>(fullNum);
  return val;
}

template <class T>
void decodeUnicodeEscape(Input<T>& in, std::string& out) {
  auto hexVal = [&](int c) -> uint16_t {
    // clang-format off
    return uint16_t(
        c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10 :
        in.template error<uint16_t>("invalid hex digit"));
    // clang-format on
  };

  auto readHex = [&]() -> uint16_t {
    if (in.size() < 4) {
      in.error("expected 4 hex digits");
    }

    auto ret = uint16_t(hexVal(*in) * 4096);
    ++in;
    ret += hexVal(*in) * 256;
    ++in;
    ret += hexVal(*in) * 16;
    ++in;
    ret += hexVal(*in);
    ++in;
    return ret;
  };

  //  If the value encoded is in the surrogate pair range, we need to make
  //  sure there is another escape that we can use also.
  //
  //  See the explanation in folly/Unicode.h.
  uint16_t prefix = readHex();
  char32_t codePoint = prefix;
  if (utf16_code_unit_is_high_surrogate(prefix)) {
    if (!in.consume("\\u")) {
      in.error(
          "expected another unicode escape for second half of "
          "surrogate pair");
    }
    uint16_t suffix = readHex();
    if (!utf16_code_unit_is_low_surrogate(suffix)) {
      in.error("second character in surrogate pair is invalid");
    }
    codePoint = unicode_code_point_from_utf16_surrogate_pair(prefix, suffix);
  } else if (!utf16_code_unit_is_bmp(prefix)) {
    in.error("invalid unicode code point (in range [0xdc00,0xdfff])");
  }

  appendCodePointToUtf8(codePoint, out);
}

// Continue after firstRun, already consumed and accounted for.
// Copy the borrowed span once and decode into an owned string.
// Consume the closing quote on success.
template <class T>
FOLLY_NOINLINE std::string parseStringEscapedTail(
    Input<T>& in, char quoteChar, std::string_view firstRun) {
  const bool json5 = in.getOpts().allow_json5_experimental;
  const auto scanBody = [&] {
    const auto range = in.skipStringBodyAfterEscape(quoteChar);
    return std::string_view(range.data(), range.size());
  };
  std::string ret;
  for (auto range = firstRun;; range = scanBody()) {
    ret.append(range.begin(), range.end());

    if (json5 && (*in == '\n' || *in == '\r')) {
      in.error("unescaped newline in string");
    }

    if (*in == quoteChar) {
      ++in;
      break;
    }
    if (*in == '\\') {
      ++in;

      bool consumed = false;
      for (std::string_view next : {"\r\n", "\r", "\n", "'"}) {
        if (json5 && in.consume(next)) {
          consumed = true;
          if (next == "'") {
            ret += "'";
          }
          break;
        }
      }
      if (consumed) {
        continue;
      }
      switch (*in) {
          // clang-format off
        case '\"':    ret.push_back('\"'); ++in; break;
        case '\\':    ret.push_back('\\'); ++in; break;
        case '/':     ret.push_back('/');  ++in; break;
        case 'b':     ret.push_back('\b'); ++in; break;
        case 'f':     ret.push_back('\f'); ++in; break;
        case 'n':     ret.push_back('\n'); ++in; break;
        case 'r':     ret.push_back('\r'); ++in; break;
        case 't':     ret.push_back('\t'); ++in; break;
        case 'u':     ++in; decodeUnicodeEscape(in, ret); break;
        // clang-format on
        default:
          in.error(
              to<std::string>("unknown escape ", *in, " in string").c_str());
      }
      continue;
    }
    if (*in == EOF) {
      in.error("unterminated string");
    }
    if (!*in) {
      /*
       * Apparently we're actually supposed to ban all control
       * characters from strings.  This seems unnecessarily
       * restrictive, so we're only banning zero bytes.  (Since the
       * string is presumed to be UTF-8 encoded it's fine to just
       * check this way.)
       */
      in.error("null byte in string");
    }

    ret.push_back(char(*in));
    ++in;
  }

  return ret;
}

template <class T>
std::string parseString(Input<T>& in, char quoteChar) {
  DCHECK_EQ(*in, quoteChar);
  const char* const bytes = in.begin();
  const auto n = in.size();
  const auto plain = [quoteChar](char c) {
    return static_cast<uint8_t>(c) >= 0x20 && c != quoteChar && c != '\\';
  };
  if (n >= 2 && bytes[1] == quoteChar) {
    in.advance(2);
    return {};
  }
  if (n >= 3 && bytes[2] == quoteChar && plain(bytes[1])) {
    std::string ret(1, bytes[1]);
    in.advance(3);
    return ret;
  }
  if (n >= 4 && bytes[3] == quoteChar && plain(bytes[1]) && plain(bytes[2])) {
    std::string ret(bytes + 1, 2);
    in.advance(4);
    return ret;
  }
  ++in;
  auto range = in.skipStringBody(quoteChar);
  if (LIKELY(*in == quoteChar)) {
    auto ret = range.str();
    ++in;
    return ret;
  }

  return parseStringEscapedTail(in, quoteChar, {range.data(), range.size()});
}

template <class T>
dynamic parseValue(Input<T>& in, json::metadata_map* map) {
  RecursionGuard guard(in);
  const auto json5 = in.getOpts().allow_json5_experimental;

  in.skipWhitespace();
  // clang-format off
  return
      *in == '[' ? parseArray(in, map) :
      *in == '{' ? parseObject(in, map) :
      *in == '\"' ? parseString(in) :
      (*in == '\'' && json5) ?  parseString(in, '\'') :
      (*in == '-' || (*in >= '0' && *in <= '9')) ? parseNumber(in) :
      (*in == '+' && json5) ? parseNumber(in) :
      (*in == '.' && json5) ? parseNumber(in) :
      in.consume("true") ? true :
      in.consume("false") ? false :
      in.consume("null") ? nullptr :
      in.consume("Infinity") ?
      (in.getOpts().parse_numbers_as_strings ? (dynamic)"Infinity" :
        (dynamic)std::numeric_limits<double>::infinity()) :
      in.consume("NaN") ?
        (in.getOpts().parse_numbers_as_strings ? (dynamic)"NaN" :
          (dynamic)std::numeric_limits<double>::quiet_NaN()) :
      in.template error<dynamic>("expected json value");
  // clang-format on
}

} // namespace

//////////////////////////////////////////////////////////////////////

std::array<uint64_t, 2> buildExtraAsciiToEscapeBitmap(StringPiece chars) {
  std::array<uint64_t, 2> escapes{{0, 0}};
  for (auto b : ByteRange(chars)) {
    if (b >= 0x20 && b < 0x80) {
      escapes[b / 64] |= uint64_t(1) << (b % 64);
    }
  }
  return escapes;
}

std::string serialize(dynamic const& dyn, serialization_opts const& opts) {
  std::string ret;
  if (opts.pretty_formatting) {
    PrinterImpl<true> p(ret, &opts);
    p(dyn, nullptr);
  } else {
    PrinterImpl<false> p(ret, &opts);
    p(dyn, nullptr);
  }
  return ret;
}

// Fast path to determine the longest prefix that can be left
// unescaped in a string of sizeof(T) bytes packed in an integer of
// type T.
template <bool EnableExtraAsciiEscapes, class T>
size_t firstEscapableInWord(T s, const serialization_opts& opts) {
  static_assert(std::is_unsigned<T>::value, "Unsigned integer required");
  static constexpr T kOnes = ~T() / 255; // 0x...0101
  static constexpr T kMsbs = kOnes * 0x80; // 0x...8080

  // Sets the MSB of bytes < b. Precondition: b < 128.
  auto isLess = [](T w, uint8_t b) {
    // A byte is < b iff subtracting b underflows, so we check that
    // the MSB wasn't set before and it's set after the subtraction.
    return (w - kOnes * b) & ~w & kMsbs;
  };

  auto isChar = [&](uint8_t c) {
    // A byte is == c iff it is 0 if xor'd with c.
    return isLess(s ^ (kOnes * c), 1);
  };

  // The following masks have the MSB set for each byte of the word
  // that satisfies the corresponding condition.
  auto isHigh = s & kMsbs; // >= 128
  auto isLow = isLess(s, 0x20); // <= 0x1f
  auto needsEscape = isHigh | isLow | isChar('\\') | isChar('"');

  if constexpr (EnableExtraAsciiEscapes) {
    // Deal with optional bitmap for unicode escapes. Escapes can optionally be
    // set for ascii characters 32 - 127, so the inner loop may run up to 96
    // times. However, for the case where 0 or a handful of bits are set,
    // looping will be minimal through use of findFirstSet.
    for (size_t i = 0, e = opts.extra_ascii_to_escape_bitmap.size(); i < e;
         ++i) {
      const auto offset = i * 64;
      // Clear first 32 characters if this is the first index, since those are
      // always escaped.
      auto bitmap = opts.extra_ascii_to_escape_bitmap[i] &
          (i == 0 ? uint64_t(-1) << 32 : ~0ULL);
      while (bitmap) {
        auto bit = folly::findFirstSet(bitmap);
        needsEscape |= isChar(static_cast<uint8_t>(offset + bit - 1));
        bitmap &= bitmap - 1;
      }
    }
  }

  if (!needsEscape) {
    return sizeof(T);
  }

  if (folly::kIsLittleEndian) {
    return folly::findFirstSet(needsEscape) / 8 - 1;
  } else {
    return sizeof(T) - folly::findLastSet(needsEscape) / 8;
  }
}

// Escape a string so that it is legal to print it in JSON text.
template <bool EnableExtraAsciiEscapes>
void escapeStringImpl(
    StringPiece input, std::string& out, const serialization_opts& opts) {
  auto hexDigit = [](uint8_t c) -> char {
    return c < 10 ? c + '0' : c - 10 + 'a';
  };

  out.push_back('\"');

  auto* p = reinterpret_cast<const unsigned char*>(input.begin());
  auto* q = reinterpret_cast<const unsigned char*>(input.begin());
  auto* e = reinterpret_cast<const unsigned char*>(input.end());

  while (p < e) {
    // Find the longest prefix that does not need escaping, and copy
    // it literally into the output string.
    auto firstEsc = p;
    while (firstEsc < e) {
      auto avail = to_unsigned(e - firstEsc);
      uint64_t word = 0;
      if (avail >= 8) {
        word = folly::loadUnaligned<uint64_t>(firstEsc);
      } else {
        word = folly::partialLoadUnaligned<uint64_t>(firstEsc, avail);
      }
      auto prefix = firstEscapableInWord<EnableExtraAsciiEscapes>(word, opts);
      DCHECK_LE(prefix, avail);
      firstEsc += prefix;
      if (prefix < 8) {
        break;
      }
    }
    if (firstEsc > p) {
      out.append(reinterpret_cast<const char*>(p), firstEsc - p);
      p = firstEsc;
      // We can't be in the middle of a multibyte sequence, so we can reset q.
      q = p;
      if (p == e) {
        break;
      }
    }

    // Handle the next byte that may need escaping.

    // Since non-ascii encoding inherently does utf8 validation
    // we explicitly validate utf8 only if non-ascii encoding is disabled.
    if ((opts.validate_utf8 || opts.skip_invalid_utf8) &&
        !opts.encode_non_ascii) {
      // To achieve better spatial and temporal coherence
      // we do utf8 validation progressively along with the
      // string-escaping instead of two separate passes.

      // As the encoding progresses, q will stay at or ahead of p.
      CHECK_GE(q, p);

      // As p catches up with q, move q forward.
      if (q == p) {
        // calling utf8_decode has the side effect of
        // checking that utf8 encodings are valid
        char32_t v = utf8ToCodePoint(q, e, opts.skip_invalid_utf8);
        if (opts.skip_invalid_utf8 && v == U'\ufffd') {
          out.append(reinterpret_cast<const char*>(u8"\ufffd"));
          p = q;
          continue;
        }
      }
    }

    auto encodeUnicode = opts.encode_non_ascii && (*p & 0x80);
    if constexpr (EnableExtraAsciiEscapes) {
      encodeUnicode = encodeUnicode ||
          (*p >= 0x20 && *p < 0x80 &&
           (opts.extra_ascii_to_escape_bitmap[*p / 64] &
            (uint64_t(1) << (*p % 64))));
    }

    if (encodeUnicode) {
      // note that this if condition captures utf8 chars
      // with value > 127, so size > 1 byte (or they are whitelisted for
      // Unicode encoding).
      // NOTE: char32_t / char16_t are both unsigned.
      char32_t cp = utf8ToCodePoint(p, e, opts.skip_invalid_utf8);
      auto writeHex = [&](char16_t v) {
        char buf[] = "\\u\0\0\0\0";
        buf[2] = hexDigit((v >> 12) & 0x0f);
        buf[3] = hexDigit((v >> 8) & 0x0f);
        buf[4] = hexDigit((v >> 4) & 0x0f);
        buf[5] = hexDigit(v & 0x0f);
        out.append(buf, 6);
      };
      // From the ECMA-404 The JSON Data Interchange Syntax 2nd Edition Dec 2017
      if (cp < 0x10000u) {
        // If the code point is in the Basic Multilingual Plane (U+0000 through
        // U+FFFF), then it may be represented as a six-character sequence:
        // a reverse solidus, followed by the lowercase letter u, followed by
        // four hexadecimal digits that encode the code point.
        writeHex(static_cast<char16_t>(cp));
      } else {
        // To escape a code point that is not in the Basic Multilingual Plane,
        // the character may be represented as a twelve-character sequence,
        // encoding the UTF-16 surrogate pair corresponding to the code point.
        writeHex(
            static_cast<char16_t>(
                0xd800u + (((cp - 0x10000u) >> 10) & 0x3ffu)));
        writeHex(static_cast<char16_t>(0xdc00u + ((cp - 0x10000u) & 0x3ffu)));
      }
    } else if (*p == '\\' || *p == '\"') {
      char buf[] = "\\\0";
      buf[1] = char(*p++);
      out.append(buf, 2);
    } else if (*p <= 0x1f) {
      switch (*p) {
          // clang-format off
        case '\b': out.append("\\b"); p++; break;
        case '\f': out.append("\\f"); p++; break;
        case '\n': out.append("\\n"); p++; break;
        case '\r': out.append("\\r"); p++; break;
        case '\t': out.append("\\t"); p++; break;
        // clang-format on
        default:
          // Note that this if condition captures non readable chars
          // with value < 32, so size = 1 byte (e.g control chars).
          char buf[] = "\\u00\0\0";
          buf[4] = hexDigit(uint8_t((*p & 0xf0) >> 4));
          buf[5] = hexDigit(uint8_t(*p & 0xf));
          out.append(buf, 6);
          p++;
      }
    } else {
      out.push_back(char(*p++));
    }
  }

  out.push_back('\"');
}

// Fast path for short ASCII-only strings with no special characters.
//
// On bail-out we fall through to escapeStringImpl, which handles the
// general case identically to before.
//
// Caller must have verified that `extra_ascii_to_escape_bitmap` is empty
// and `encode_non_ascii` is false.
//
// Limit chosen to fit comfortably in a small stack buffer and to cover
// typical Scuba/Thrift identifier and short-value lengths.
constexpr size_t kEscapeFastPathMax = 32;

FOLLY_ALWAYS_INLINE bool tryEscapeShortAscii(
    StringPiece input, std::string& out) {
  const size_t n = input.size();
  if (n > kEscapeFastPathMax) {
    return false;
  }
  char buf[kEscapeFastPathMax + 2];
  buf[0] = '"';
  // OR-reduce a "needs escape" mask while copying. Branchless inner body
  // lets the compiler vectorise / interleave loads with the OR.
  uint8_t bad = 0;
  for (size_t i = 0; i < n; ++i) {
    const auto u = static_cast<unsigned char>(input[i]);
    bad |= (u < 0x20) | (u >> 7) | (u == '"') | (u == '\\');
    buf[i + 1] = static_cast<char>(u);
  }
  if (FOLLY_UNLIKELY(bad)) {
    return false;
  }
  buf[n + 1] = '"';
  out.append(buf, n + 2);
  return true;
}

void escapeString(
    StringPiece input, std::string& out, const serialization_opts& opts) {
  if (FOLLY_LIKELY(
          opts.extra_ascii_to_escape_bitmap[0] == 0 &&
          opts.extra_ascii_to_escape_bitmap[1] == 0 &&
          !opts.encode_non_ascii)) {
    if (tryEscapeShortAscii(input, out)) {
      return;
    }
    escapeStringImpl<false>(input, out, opts);
    return;
  }
  escapeStringImpl<true>(input, out, opts);
}

std::string stripComments(StringPiece jsonC) {
  std::string result;
  enum class State {
    None,
    InString,
    InlineComment,
    LineComment
  } state = State::None;

  for (size_t i = 0; i < jsonC.size(); ++i) {
    auto s = jsonC.subpiece(i);
    switch (state) {
      case State::None:
        if (s.startsWith("/*")) {
          state = State::InlineComment;
          ++i;
          continue;
        } else if (s.startsWith("//")) {
          state = State::LineComment;
          ++i;
          continue;
        } else if (s[0] == '\"') {
          state = State::InString;
        }
        result.push_back(s[0]);
        break;
      case State::InString:
        if (s[0] == '\\') {
          if (FOLLY_UNLIKELY(s.size() == 1)) {
            throw std::logic_error("Invalid JSONC: string is not terminated");
          }
          result.push_back(s[0]);
          result.push_back(s[1]);
          ++i;
          continue;
        } else if (s[0] == '\"') {
          state = State::None;
        }
        result.push_back(s[0]);
        break;
      case State::InlineComment:
        if (s.startsWith('\n')) {
          // preserve the line break to preserve the line count
          result.push_back(s[0]);
        } else if (s.startsWith("*/")) {
          state = State::None;
          ++i;
        }
        break;
      case State::LineComment:
        if (s[0] == '\n') {
          // preserve the line break to preserve the line count
          result.push_back(s[0]);
          state = State::None;
        }
        break;
      default:
        throw std::logic_error("Unknown comment state");
    }
  }
  return result;
}

} // namespace json

//////////////////////////////////////////////////////////////////////

namespace {

template <class OptsPtr>
dynamic parseJsonImpl(
    StringPiece range, OptsPtr p, json::metadata_map* map = nullptr) {
  json::Input in{range, p};
  uint32_t n = in.getLineNum();
  auto ret = parseValue(in, map);
  if (map) {
    map->emplace(&ret, json::parse_metadata{{{0}}, {{n}}});
  }

  in.skipWhitespace();
  if (in.size() && *in != '\0') {
    in.error("parsing didn't consume all input");
  }
  return ret;
}

} // namespace

dynamic parseJsonWithMetadata(StringPiece range, json::metadata_map* map) {
  return parseJsonImpl(range, json::DefaultOptsPtr{}, map);
}

dynamic parseJsonWithMetadata(
    StringPiece range,
    json::serialization_opts const& opts,
    json::metadata_map* map) {
  return parseJsonImpl(range, &opts, map);
}

dynamic parseJson(StringPiece range) {
  return parseJsonWithMetadata(range, nullptr);
}

dynamic parseJson5(StringPiece range) {
  json::serialization_opts opts;
  opts.allow_json5_experimental = true;
  return parseJson(range, opts);
}

dynamic parseJson(StringPiece range, json::serialization_opts const& opts) {
  return parseJsonImpl(range, &opts);
}

std::string toJson(dynamic const& dyn) {
  return json::serialize(dyn, json::serialization_opts());
}

std::string toPrettyJson(dynamic const& dyn) {
  json::serialization_opts opts;
  opts.pretty_formatting = true;
  opts.sort_keys = true;
  return json::serialize(dyn, opts);
}

//////////////////////////////////////////////////////////////////////
// dynamic::print_as_pseudo_json() is implemented here for header
// ordering reasons (most of the dynamic implementation is in
// dynamic-inl.h, which we don't want to include json.h).

void dynamic::print_as_pseudo_json(std::ostream& out) const {
  json::serialization_opts opts;
  opts.allow_non_string_keys = true;
  opts.allow_nan_inf = true;
  out << json::serialize(*this, opts);
}

void PrintTo(const dynamic& dyn, std::ostream* os) {
  json::serialization_opts opts;
  opts.allow_nan_inf = true;
  opts.allow_non_string_keys = true;
  opts.pretty_formatting = true;
  opts.sort_keys = true;
  *os << json::serialize(dyn, opts);
}

//////////////////////////////////////////////////////////////////////

} // namespace folly
