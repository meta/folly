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

#include <iterator>
#include <stdexcept>

#include <folly/CppAttributes.h>
#include <folly/container/Reserve.h>

#ifndef FOLLY_STRING_H_
#error This file may only be included from String.h
#endif

namespace folly {

namespace detail {
// Map from character code to value of one-character escape sequence
// ('\n' = 10 maps to 'n'), 'O' if the character should be printed as
// an octal escape sequence, or 'P' if the character is printable and
// should be printed as is.
extern const std::array<char, 256> cEscapeTable;
} // namespace detail

template <class String>
void cEscape(StringPiece str, String& out) {
  char esc[4];
  esc[0] = '\\';
  grow_capacity_by(out, str.size());
  auto p = str.begin();
  auto last = p; // last regular character
  // We advance over runs of regular characters (printable, not double-quote or
  // backslash) and copy them in one go; this is faster than calling push_back
  // repeatedly.
  while (p != str.end()) {
    char c = *p;
    unsigned char v = static_cast<unsigned char>(c);
    char e = detail::cEscapeTable[v];
    if (e == 'P') { // printable
      ++p;
    } else if (e == 'O') { // octal
      out.append(&*last, size_t(p - last));
      esc[1] = '0' + ((v >> 6) & 7);
      esc[2] = '0' + ((v >> 3) & 7);
      esc[3] = '0' + (v & 7);
      out.append(esc, 4);
      ++p;
      last = p;
    } else { // special 1-character escape
      out.append(&*last, size_t(p - last));
      esc[1] = e;
      out.append(esc, 2);
      ++p;
      last = p;
    }
  }
  out.append(&*last, size_t(p - last));
}

namespace detail {
// Map from the character code of the character following a backslash to
// the unescaped character if a valid one-character escape sequence
// ('n' maps to 10 = '\n'), 'O' if this is the first character of an
// octal escape sequence, 'X' if this is the first character of a
// hexadecimal escape sequence, or 'I' if this escape sequence is invalid.
extern const std::array<char, 256> cUnescapeTable;

// Map from the character code to the hex value, or 16 if invalid hex char.
extern const std::array<unsigned char, 256> hexTable;
} // namespace detail

template <class String>
void cUnescape(StringPiece str, String& out, bool strict) {
  grow_capacity_by(out, str.size());
  auto p = str.begin();
  auto last = p; // last regular character (not part of an escape sequence)
  // We advance over runs of regular characters (not backslash) and copy them
  // in one go; this is faster than calling push_back repeatedly.
  while (p != str.end()) {
    char c = *p;
    if (c != '\\') { // normal case
      ++p;
      continue;
    }
    out.append(&*last, p - last);
    ++p;
    if (p == str.end()) { // backslash at end of string
      if (strict) {
        throw_exception<std::invalid_argument>("incomplete escape sequence");
      }
      out.push_back('\\');
      last = p;
      continue;
    }
    char e = detail::cUnescapeTable[static_cast<unsigned char>(*p)];
    if (e == 'O') { // octal
      unsigned char val = 0;
      for (int i = 0; i < 3 && p != str.end() && *p >= '0' && *p <= '7';
           ++i, ++p) {
        val <<= 3;
        val |= (*p - '0');
      }
      out.push_back(val);
      last = p;
    } else if (e == 'X') { // hex
      ++p;
      if (p == str.end() ||
          detail::hexTable[static_cast<unsigned char>(*p)] >= 16) {
        // \x not followed by a hex digit
        if (strict) {
          throw_exception<std::invalid_argument>(
              "incomplete hex escape sequence");
        }
        out.append("\\x");
        last = p;
        continue;
      }
      unsigned char val = 0;
      unsigned char h;
      for (; (p != str.end() &&
              (h = detail::hexTable[static_cast<unsigned char>(*p)]) < 16);
           ++p) {
        val <<= 4;
        val |= h;
      }
      out.push_back(val);
      last = p;
    } else if (e == 'I') { // invalid
      if (strict) {
        throw_exception<std::invalid_argument>("invalid escape sequence");
      }
      out.push_back('\\');
      out.push_back(*p);
      ++p;
      last = p;
    } else { // standard escape sequence, \' etc
      out.push_back(e);
      ++p;
      last = p;
    }
  }
  out.append(&*last, p - last);
}

namespace detail {
// Map from character code to escape mode:
// 0 = pass through
// 1 = unused
// 2 = pass through in PATH mode
// 3 = space, replace with '+' in QUERY mode
// 4 = percent-encode
extern const std::array<unsigned char, 256> uriEscapeTable;
} // namespace detail

template <class String>
void uriEscape(StringPiece str, String& out, UriEscapeMode mode) {
  static const char hexValues[] = "0123456789abcdef";
  char esc[3];
  esc[0] = '%';
  // Preallocate assuming that 25% of the input string will be escaped
  grow_capacity_by(out, str.size() + 3 * (str.size() / 4));
  auto p = str.begin();
  auto last = p; // last regular character
  // We advance over runs of passthrough characters and copy them in one go;
  // this is faster than calling push_back repeatedly.
  unsigned char minEncode = static_cast<unsigned char>(mode);
  while (p != str.end()) {
    char c = *p;
    unsigned char v = static_cast<unsigned char>(c);
    unsigned char discriminator = detail::uriEscapeTable[v];
    if (discriminator <= minEncode) [[likely]] {
      ++p;
    } else if (mode == UriEscapeMode::QUERY && discriminator == 3) {
      out.append(&*last, size_t(p - last));
      out.push_back('+');
      ++p;
      last = p;
    } else {
      out.append(&*last, size_t(p - last));
      esc[1] = hexValues[v >> 4];
      esc[2] = hexValues[v & 0x0f];
      out.append(esc, 3);
      ++p;
      last = p;
    }
  }
  out.append(&*last, size_t(p - last));
}

template <class String>
bool tryUriUnescape(StringPiece str, String& out, UriEscapeMode mode) {
  grow_capacity_by(out, str.size());
  auto p = str.begin();
  auto last = p;
  // We advance over runs of passthrough characters and copy them in one go;
  // this is faster than calling push_back repeatedly.
  while (p != str.end()) {
    char c = *p;
    switch (c) {
      case '%': {
        if (std::distance(p, str.end()) < 3) [[unlikely]] {
          return false;
        }
        auto h1 = detail::hexTable[static_cast<unsigned char>(p[1])];
        auto h2 = detail::hexTable[static_cast<unsigned char>(p[2])];
        if (h1 == 16 || h2 == 16) [[unlikely]] {
          return false;
        }
        out.append(&*last, size_t(p - last));
        out.push_back(decltype(h1)(h1 << 4) | h2);
        p += 3;
        last = p;
        break;
      }
      case '+':
        if (mode == UriEscapeMode::QUERY) {
          out.append(&*last, size_t(p - last));
          out.push_back(' ');
          ++p;
          last = p;
          break;
        }
        // else fallthrough
        [[fallthrough]];
      default:
        ++p;
        break;
    }
  }
  out.append(&*last, size_t(p - last));

  return true;
}

template <class String>
void uriUnescape(StringPiece str, String& out, UriEscapeMode mode) {
  auto success = tryUriUnescape(str, out, mode);

  if (!success) {
    // tryUriEscape implementation only fails on invalid argument
    throw_exception<std::invalid_argument>(
        "incomplete percent encode sequence");
  }
}

namespace detail {

/*
 * The following functions are type-overloaded helpers for
 * internalSplit().
 */
inline size_t delimSize(char) {
  return 1;
}
inline size_t delimSize(StringPiece s) {
  return s.size();
}
inline bool atDelim(const char* s, char c) {
  return *s == c;
}
inline bool atDelim(const char* s, StringPiece sp) {
  return !std::memcmp(s, sp.start(), sp.size());
}

// These are used to short-circuit internalSplit() in the case of
// 1-character strings.
inline char delimFront(char) {
  // This one exists only for compile-time; it should never be called.
  std::abort();
}
inline char delimFront(StringPiece s) {
  assert(!s.empty() && s.start() != nullptr);
  return *s.start();
}

template <class OutStringT, class DelimT, class OutputIterator>
void internalSplit(
    DelimT delim, StringPiece sp, OutputIterator out, bool ignoreEmpty);

/*
 * Count the number of tokens that would be produced by splitting
 * the given StringPiece with the given delimiter.
 * This is used for pre-allocation optimization in split().
 */
size_t delimCountTokens(char delim, StringPiece sp, bool ignoreEmpty);
size_t delimCountTokens(StringPiece delim, StringPiece sp, bool ignoreEmpty);

template <class OutStringT, class Container>
  requires(
      IsSplitSupportedContainer<Container>::value &&
      HasSimdSplitCompatibleValueType<Container>::value)
void internalSplitRecurseChar(
    char delim,
    folly::StringPiece sp,
    std::back_insert_iterator<Container> it,
    bool ignoreEmpty) {
  using base = std::back_insert_iterator<Container>;
  struct accessor : base {
    accessor(base b) : base(b) {}
    using base::container;
  };
  detail::simdSplitByChar(delim, sp, *accessor{it}.container, ignoreEmpty);
}

template <class OutStringT, class Iterator>
void internalSplitRecurseChar(
    char delim, folly::StringPiece sp, Iterator it, bool ignoreEmpty) {
  internalSplit<OutStringT>(delim, sp, it, ignoreEmpty);
}

/*
 * Shared implementation for all the split() overloads.
 *
 * This uses some external helpers that are overloaded to let this
 * algorithm be more performant if the deliminator is a single
 * character instead of a whole string.
 *
 * @param ignoreEmpty if true, don't copy empty segments to output
 */
template <class OutStringT, class DelimT, class OutputIterator>
void internalSplit(
    DelimT delim, StringPiece sp, OutputIterator out, bool ignoreEmpty) {
  assert(sp.empty() || sp.start() != nullptr);

  const char* s = sp.start();
  const size_t strSize = sp.size();
  const size_t dSize = delimSize(delim);

  if (dSize > strSize || dSize == 0) {
    if (!ignoreEmpty || strSize > 0) {
      *out++ = to<OutStringT>(sp);
    }
    return;
  }
  if (std::is_same<DelimT, StringPiece>::value && dSize == 1) {
    // Call the char version because it is significantly faster.
    return internalSplitRecurseChar<OutStringT>(
        delimFront(delim), sp, out, ignoreEmpty);
  }

  size_t tokenStartPos = 0;
  size_t tokenSize = 0;
  for (size_t i = 0; i <= strSize - dSize; ++i) {
    if (atDelim(&s[i], delim)) {
      if (!ignoreEmpty || tokenSize > 0) {
        *out++ = to<OutStringT>(sp.subpiece(tokenStartPos, tokenSize));
      }

      tokenStartPos = i + dSize;
      tokenSize = 0;
      i += dSize - 1;
    } else {
      ++tokenSize;
    }
  }
  tokenSize = strSize - tokenStartPos;
  if (!ignoreEmpty || tokenSize > 0) {
    *out++ = to<OutStringT>(sp.subpiece(tokenStartPos, tokenSize));
  }
}

template <class String>
StringPiece prepareDelim(const String& s) {
  return StringPiece(s);
}
inline char prepareDelim(char c) {
  return c;
}

template <class OutputType>
void toOrIgnore(StringPiece input, OutputType& output) {
  output = folly::to<OutputType>(input);
}

inline void toOrIgnore(StringPiece, decltype(std::ignore)&) {}

template <bool exact, class Delim, class OutputType>
bool splitFixed(const Delim& delimiter, StringPiece input, OutputType& output) {
  if (exact && FOLLY_UNLIKELY(std::string::npos != input.find(delimiter))) {
    return false;
  }
  toOrIgnore(input, output);
  return true;
}

template <bool exact, class Delim, class OutputType, class... OutputTypes>
bool splitFixed(
    const Delim& delimiter,
    StringPiece input,
    OutputType& outHead,
    OutputTypes&... outTail) {
  size_t cut = input.find(delimiter);
  if (cut == std::string::npos) [[unlikely]] {
    return false;
  }
  StringPiece head(input.begin(), input.begin() + cut);
  StringPiece tail(
      input.begin() + cut + detail::delimSize(delimiter), input.end());
  if (splitFixed<exact>(delimiter, tail, outTail...)) [[likely]] {
    toOrIgnore(head, outHead);
    return true;
  }
  return false;
}

// Overload for no remaining output fields; requires empty input.
template <class Delim>
Expected<Unit, SubstringConversionCode> trySplitTo(
    StringPiece input, const Delim&) {
  if (input.empty()) {
    return unit;
  }
  return makeUnexpected(
      SubstringConversionCode{input, ConversionCode::SPLIT_ERROR});
}

// Replace custom conversion codes with folly::ConversionCode::CUSTOM_OTHER.
template <class CustomCode>
inline ConversionCode convertError(CustomCode&&) {
  return ConversionCode::CUSTOM;
}

inline ConversionCode convertError(ConversionCode code) {
  return code;
}

// tryFieldTo helpers, wrapping tryTo<>, but adding support for std::ignore and
// replacing custom error types with ConversionCode::CUSTOM.
template <class Output>
Expected<Output, ConversionCode> tryFieldTo(folly::StringPiece input) {
  if (auto result = tryTo<Output>(input)) {
    return std::move(result.value());
  } else {
    return makeUnexpected(convertError(result.error()));
  }
}

template <>
inline Expected<decltype(std::ignore), ConversionCode>
tryFieldTo<decltype(std::ignore)>(folly::StringPiece /*input*/) {
  return std::ignore;
}

template <class Delim, class Output, class... Outputs>
Expected<Unit, SubstringConversionCode> trySplitTo(
    StringPiece input,
    const Delim& delim,
    Output& output,
    Outputs&... outputs) {
  auto pos = input.find(delim);
  if ((pos == std::string::npos) != (sizeof...(outputs) == 0)) {
    return makeUnexpected(
        SubstringConversionCode{input, ConversionCode::SPLIT_ERROR});
  }
  StringPiece head, tail;
  if (pos == std::string::npos) {
    head = input;
  } else {
    head = input.subpiece(0, pos);
    tail = input.subpiece(pos + delimSize(delim));
  }
  // Eagerly attempt parsing the head value, but only assign on the way back
  // from the recursive calls to ensure all outputs are untouched on failure.
  if (auto headResult = tryFieldTo<Output>(head)) {
    if (auto tailResult = trySplitTo(tail, delim, outputs...)) {
      output = *headResult;
      return unit;

    } else {
      return makeUnexpected(tailResult.error());
    }
  } else {
    // First failure (left-to-right) is returned.
    return makeUnexpected(SubstringConversionCode{head, headResult.error()});
  }
}

} // namespace detail

//////////////////////////////////////////////////////////////////////

template <class Delim, class String, class OutputType>
  requires(
      (!detail::IsSimdSupportedDelim<Delim>::value ||
       !detail::HasSimdSplitCompatibleValueType<OutputType>::value) &&
      detail::IsSplitSupportedContainer<OutputType>::value)
void split(
    const Delim& delimiter,
    const String& input,
    OutputType& out,
    bool ignoreEmpty) {
  detail::internalSplit<typename OutputType::value_type>(
      detail::prepareDelim(delimiter),
      StringPiece(input),
      std::back_inserter(out),
      ignoreEmpty);
}

template <class Delim, class String, class OutputType>
  requires detail::IsSplitSupportedContainer<OutputType>::value
void split(
    const Delim& delimiter,
    const String& input,
    OutputType& out,
    const SplitOptions& options) {
  size_t predicted_count = 0;
  if (options.preallocate()) {
    // First pass: count the expected number of tokens
    StringPiece sp(input);
    auto delim_prepared = detail::prepareDelim(delimiter);
    predicted_count =
        detail::delimCountTokens(delim_prepared, sp, options.ignore_empty());
    grow_capacity_by(out, predicted_count);
  }

  size_t initial_size = out.size();
  detail::internalSplit<typename OutputType::value_type>(
      detail::prepareDelim(delimiter),
      StringPiece(input),
      std::back_inserter(out),
      options.ignore_empty());

  if (options.preallocate()) {
    [[maybe_unused]] size_t actual_count = out.size() - initial_size;
    assert(predicted_count == actual_count);
  }
}

template <
    class OutputValueType,
    class Delim,
    class String,
    class OutputIterator>
void splitTo(
    const Delim& delimiter,
    const String& input,
    OutputIterator out,
    bool ignoreEmpty) {
  detail::internalSplit<OutputValueType>(
      detail::prepareDelim(delimiter), StringPiece(input), out, ignoreEmpty);
}

template <class Delim, class... OutputTypes>
  requires StrictConjunction<IsConvertible<OutputTypes>...>::value
Expected<Unit, SubstringConversionCode> trySplitTo(
    StringPiece input, const Delim& delim, OutputTypes&... outputs) {
  return detail::trySplitTo(input, detail::prepareDelim(delim), outputs...);
}

template <bool exact, class Delim, class... OutputTypes>
  requires(
      StrictConjunction<IsConvertible<OutputTypes>...>::value &&
      sizeof...(OutputTypes) >= 1)
bool split(const Delim& delimiter, StringPiece input, OutputTypes&... outputs) {
  return detail::splitFixed<exact>(
      detail::prepareDelim(delimiter), input, outputs...);
}

namespace detail {

/*
 * If a type can have its string size determined cheaply, we can more
 * efficiently append it in a loop (see internalJoinAppend). Note that the
 * struct need not conform to the std::string api completely (ex. does not need
 * to implement append()).
 */
template <class T>
concept IsSizableString =
    IsSomeString<T>::value || std::same_as<T, StringPiece>;

template <class Iterator>
concept IsSizableStringContainerIterator =
    IsSizableString<typename std::iterator_traits<Iterator>::value_type>;

template <class Delim, class Iterator, class String>
void internalJoinAppend(
    Delim delimiter, Iterator begin, Iterator end, String& output) {
  assert(begin != end);
  if (std::is_same<Delim, StringPiece>::value && delimSize(delimiter) == 1) {
    internalJoinAppend(delimFront(delimiter), begin, end, output);
    return;
  }
  toAppend(*begin, &output);
  while (++begin != end) {
    toAppend(delimiter, *begin, &output);
  }
}

template <class Delim, class Iterator, class String>
  requires(IsSizableStringContainerIterator<Iterator>)
void internalJoin(
    Delim delimiter, Iterator begin, Iterator end, String& output) {
  output.clear();
  if (begin == end) {
    return;
  }
  const size_t dsize = delimSize(delimiter);
  Iterator it = begin;
  size_t size = (*it).size();
  while (++it != end) {
    size += dsize + (*it).size();
  }
  output.reserve(size);
  internalJoinAppend(delimiter, begin, end, output);
}

template <class Delim, class Iterator, class String>
  requires(!IsSizableStringContainerIterator<Iterator>)
void internalJoin(
    Delim delimiter, Iterator begin, Iterator end, String& output) {
  output.clear();
  if (begin == end) {
    return;
  }
  internalJoinAppend(delimiter, begin, end, output);
}

} // namespace detail

template <class Delim, class Iterator, class String>
void join(
    const Delim& delimiter, Iterator begin, Iterator end, String& output) {
  detail::internalJoin(detail::prepareDelim(delimiter), begin, end, output);
}

template <class OutputString>
void backslashify(
    folly::StringPiece input, OutputString& output, bool hex_style) {
  static const char hexValues[] = "0123456789abcdef";
  output.clear();
  output.reserve(3 * input.size());
  for (unsigned char c : input) {
    // less than space or greater than '~' are considered unprintable
    if (c < 0x20 || c > 0x7e || c == '\\') {
      bool hex_append = false;
      output.push_back('\\');
      if (hex_style) {
        hex_append = true;
      } else {
        if (c == '\r') {
          output += 'r';
        } else if (c == '\n') {
          output += 'n';
        } else if (c == '\t') {
          output += 't';
        } else if (c == '\a') {
          output += 'a';
        } else if (c == '\b') {
          output += 'b';
        } else if (c == '\0') {
          output += '0';
        } else if (c == '\\') {
          output += '\\';
        } else {
          hex_append = true;
        }
      }
      if (hex_append) {
        output.push_back('x');
        output.push_back(hexValues[(c >> 4) & 0xf]);
        output.push_back(hexValues[c & 0xf]);
      }
    } else {
      output += c;
    }
  }
}

template <class String1, class String2>
void humanify(const String1& input, String2& output) {
  size_t numUnprintable = 0;
  size_t numPrintablePrefix = 0;
  for (unsigned char c : input) {
    if (c < 0x20 || c > 0x7e || c == '\\') {
      ++numUnprintable;
    }
    if (numUnprintable == 0) {
      ++numPrintablePrefix;
    }
  }

  // hexlify doubles a string's size; backslashify can potentially
  // explode it by 4x.  Now, the printable range of the ascii
  // "spectrum" is around 95 out of 256 values, so a "random" binary
  // string should be around 60% unprintable.  We use a 50% heuristic
  // here, so if a string is 60% unprintable, then we just use hex
  // output.  Otherwise we backslash.
  //
  // UTF8 is completely ignored; as a result, utf8 characters will
  // likely be \x escaped (since most common glyphs fit in two bytes).
  // This is a tradeoff of complexity/speed instead of a convenience
  // that likely would rarely matter.  Moreover, this function is more
  // about displaying underlying bytes, not about displaying glyphs
  // from languages.
  if (numUnprintable == 0) {
    output = input;
  } else if (5 * numUnprintable >= 3 * input.size()) {
    // However!  If we have a "meaningful" prefix of printable
    // characters, say 20% of the string, we backslashify under the
    // assumption viewing the prefix as ascii is worth blowing the
    // output size up a bit.
    if (5 * numPrintablePrefix >= input.size()) {
      backslashify(input, output);
    } else {
      output = "0x";
      hexlify(input, output, true /* append output */);
    }
  } else {
    backslashify(input, output);
  }
}

template <class InputString, class OutputString>
bool hexlify(
    const InputString& input, OutputString& output, bool append_output) {
  if (!append_output) {
    output.clear();
  }

  static char hexValues[] = "0123456789abcdef";
  auto j = output.size();
  output.resize(2 * input.size() + output.size());
  for (size_t i = 0; i < input.size(); ++i) {
    int ch = input[i];
    output[j++] = hexValues[(ch >> 4) & 0xf];
    output[j++] = hexValues[ch & 0xf];
  }
  return true;
}

template <class InputString, class OutputString>
bool unhexlify(const InputString& input, OutputString& output) {
  if (input.size() % 2 != 0) {
    return false;
  }
  output.resize(input.size() / 2);
  int j = 0;

  for (size_t i = 0; i < input.size(); i += 2) {
    int highBits = detail::hexTable[static_cast<uint8_t>(input[i])];
    int lowBits = detail::hexTable[static_cast<uint8_t>(input[i + 1])];
    if ((highBits | lowBits) & 0x10) {
      // One of the characters wasn't a hex digit
      return false;
    }
    output[j++] = (highBits << 4) + lowBits;
  }
  return true;
}

namespace detail {
/**
 * Hex-dump at most 16 bytes starting at offset from a memory area of size
 * bytes.  Return the number of bytes actually dumped.
 */
size_t hexDumpLine(
    const void* ptr, size_t offset, size_t size, std::string& line);
} // namespace detail

template <class OutIt>
void hexDump(const void* ptr, size_t size, OutIt out) {
  size_t offset = 0;
  std::string line;
  while (offset < size) {
    offset += detail::hexDumpLine(ptr, offset, size, line);
    *out++ = line;
  }
}

} // namespace folly
