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

//
// Docs: https://fburl.com/fbcref_string
//

/**
 * Convenience functions for working with strings.
 *
 * @file String.h
 */

#pragma once
#define FOLLY_STRING_H_

#include <cstdarg>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <folly/Conv.h>
#include <folly/ExceptionString.h>
#include <folly/Optional.h>
#include <folly/Portability.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/Traits.h>
#include <folly/Unit.h>
#include <folly/detail/SimpleSimdStringUtils.h>
#include <folly/detail/SplitStringSimd.h>

namespace folly {

/** Consume a matching prefix or suffix; leave the view unchanged on mismatch.
 */
constexpr bool removePrefix(
    std::string_view& input, std::string_view prefix) noexcept {
  if (!input.starts_with(prefix)) {
    return false;
  }
  input.remove_prefix(prefix.size());
  return true;
}

constexpr bool removePrefix(std::string_view& input, char prefix) noexcept {
  if (!input.starts_with(prefix)) {
    return false;
  }
  input.remove_prefix(1);
  return true;
}

constexpr bool removeSuffix(
    std::string_view& input, std::string_view suffix) noexcept {
  if (!input.ends_with(suffix)) {
    return false;
  }
  input.remove_suffix(suffix.size());
  return true;
}

constexpr bool removeSuffix(std::string_view& input, char suffix) noexcept {
  if (!input.ends_with(suffix)) {
    return false;
  }
  input.remove_suffix(1);
  return true;
}

/**
 * Return the next field and consume it and its delimiter from input.
 * A missing delimiter consumes the entire input. An empty delimiter returns an
 * empty field without advancing input.
 */
constexpr std::string_view splitStep(
    std::string_view& input, char delimiter) noexcept {
  auto const pos = input.find(delimiter);
  auto const result = input.substr(0, pos);
  input.remove_prefix(pos == input.npos ? input.size() : pos + 1);
  return result;
}

constexpr std::string_view splitStep(
    std::string_view& input, std::string_view delimiter) noexcept {
  auto const pos = input.find(delimiter);
  auto const result = input.substr(0, pos);
  input.remove_prefix(
      pos == input.npos ? input.size() : pos + delimiter.size());
  return result;
}

/** The callback observes input after the field has been consumed. */
template <typename Delimiter, typename Process, typename... Args>
auto splitStep(
    std::string_view& input,
    Delimiter const& delimiter,
    Process&& process,
    Args&&... args)
    -> decltype(process(
        splitStep(input, delimiter), std::forward<Args>(args)...)) {
  return process(splitStep(input, delimiter), std::forward<Args>(args)...);
}

/// SplitOptions
///
/// Options for controlling split() behavior. This class uses a builder pattern
/// to allow for easy configuration and method chaining.
class SplitOptions {
 public:
  struct Defaults {
    /// preallocate
    ///
    /// If true, split will count the expected number of tokens first
    /// and pre-allocate container capacity using grow_capacity_by.
    /// This can improve performance when splitting large strings with
    /// many tokens, but may add overhead for small strings.
    static constexpr bool preallocate = false;

    /// ignoreEmpty
    ///
    /// If true, adjacent delimiters are treated as one single separator
    /// (ignoring empty tokens), otherwise empty tokens are generated.
    static constexpr bool ignore_empty = false;
  };

  constexpr bool preallocate() const { return preallocate_; }
  constexpr SplitOptions& preallocate(bool enable) {
    preallocate_ = enable;
    return *this;
  }

  constexpr bool ignore_empty() const { return ignore_empty_; }
  constexpr SplitOptions& ignore_empty(bool enable) {
    ignore_empty_ = enable;
    return *this;
  }

 private:
  bool preallocate_ = Defaults::preallocate;
  bool ignore_empty_ = Defaults::ignore_empty;
};

/**
 * @overloadbrief C-escape a string.
 *
 * Make the string suitable for representation as a C string
 * literal.  Appends the result to the output string.
 *
 * Backslashes all occurrences of backslash, double-quote, and question mark:
 *   "  ->  \"
 *   \  ->  \\
 *   ?  ->  \?
 *
 * (Question marks are escaped in order to prevent creating trigraphs in
 * the output -- "??x" where x is one of "=/'()!<>-")
 *
 * Also backslashes certain whitespace characters: \n, \r, \t
 *
 * Replaces all non-printable ASCII characters with backslash-octal
 * representation:
 *   <ASCII 254> -> \376
 *
 * Note that we use backslash-octal instead of backslash-hex because the octal
 * representation is guaranteed to consume no more than 3 characters; "\3760"
 * represents two characters, one with value 254, and one with value 48 ('0'),
 * whereas "\xfe0" represents only one character (with value 4064, which leads
 * to implementation-defined behavior).
 */
template <class String>
void cEscape(StringPiece str, String& out);

/**
 * Similar to cEscape above, but returns the escaped string.
 */
template <class String>
String cEscape(StringPiece str) {
  String out;
  cEscape(str, out);
  return out;
}

/**
 * @overloadbrief C-Unescape a string.
 *
 * The opposite of cEscape above.  Appends the result
 * to the output string.
 *
 * Recognizes the standard C escape sequences:
 *
 * \code
 * \' \" \? \\ \a \b \f \n \r \t \v
 * \[0-7]+
 * \x[0-9a-fA-F]+
 * \endcode
 *
 * In strict mode (default), throws std::invalid_argument if it encounters
 * an unrecognized escape sequence.  In non-strict mode, it leaves
 * the escape sequence unchanged.
 */
template <class String>
void cUnescape(StringPiece str, String& out, bool strict = true);

/**
 * Similar to cUnescape above, but returns the escaped string.
 */
template <class String>
String cUnescape(StringPiece str, bool strict = true) {
  String out;
  cUnescape(str, out, strict);
  return out;
}

/**
 * @overloadbrief URI-escape a string.
 *
 * Appends the result to the output string.
 *
 * Alphanumeric characters and other characters marked as "unreserved" in RFC
 * 3986 ( -_.~ ) are left unchanged.  In PATH mode, the forward slash (/) is
 * also left unchanged.  In QUERY mode, spaces are replaced by '+'.  All other
 * characters are percent-encoded.
 */
enum class UriEscapeMode : unsigned char {
  // The values are meaningful, see generate_escape_tables.py
  ALL = 0,
  QUERY = 1,
  PATH = 2
};
template <class String>
void uriEscape(
    StringPiece str, String& out, UriEscapeMode mode = UriEscapeMode::ALL);

/**
 * Similar to uriEscape above, but returns the escaped string.
 */
template <class String>
String uriEscape(StringPiece str, UriEscapeMode mode = UriEscapeMode::ALL) {
  String out;
  uriEscape(str, out, mode);
  return out;
}

/**
 * @overloadbrief URI-unescape a string.
 *
 * Appends the result to the output string.
 *
 * In QUERY mode, '+' are replaced by space.  %XX sequences are decoded if
 * XX is a valid hex sequence, otherwise we return an unexpected
 * std::invalid_argument.
 */
template <class String>
bool tryUriUnescape(
    StringPiece str, String& out, UriEscapeMode mode = UriEscapeMode::ALL);

/**
 * Similar to tryUriUnescape above, but returning the unescaped string as a
 * folly::Expected.
 */
template <class String>
folly::Optional<String> tryUriUnescape(
    StringPiece str, UriEscapeMode mode = UriEscapeMode::ALL) {
  String out;
  auto success = tryUriUnescape(str, out, mode);

  if (!success) {
    return folly::none;
  }

  return out;
}

/**
 * Similar to tryUriUnescape above, but without folly::Expected wrapping, and
 * throwing std::invalid_argument on malformed input.
 */
template <class String>
void uriUnescape(
    StringPiece str, String& out, UriEscapeMode mode = UriEscapeMode::ALL);

/**
 * Similar to uriUnescape above, but returns the unescaped string.
 */
template <class String>
String uriUnescape(StringPiece str, UriEscapeMode mode = UriEscapeMode::ALL) {
  String out;
  uriUnescape(str, out, mode);
  return out;
}

/**
 * @overloadbrief printf into a string.
 *
 * stringPrintf is much like printf but deposits its result into a
 * string. Two signatures are supported: the first simply returns the
 * resulting string, and the second appends the produced characters to
 * the specified string and returns a reference to it.
 */
std::string stringPrintf(FOLLY_PRINTF_FORMAT const char* format, ...)
    FOLLY_PRINTF_FORMAT_ATTR(1, 2);

/* Similar to stringPrintf, with different signature. */
void stringPrintf(std::string* out, FOLLY_PRINTF_FORMAT const char* format, ...)
    FOLLY_PRINTF_FORMAT_ATTR(2, 3);

/**
 * Append printf-style output to string.
 */
std::string& stringAppendf(
    std::string* output, FOLLY_PRINTF_FORMAT const char* format, ...)
    FOLLY_PRINTF_FORMAT_ATTR(2, 3);

/**
 * @overloadbrief stringPrintf with va_list argument
 *
 * As with vsnprintf() itself, the value of ap is undefined after the call.
 * These functions do not call va_end() on ap.
 */
std::string stringVPrintf(const char* format, va_list ap);
void stringVPrintf(std::string* out, const char* format, va_list ap);

/**
 * Append va_list printf-style output to string.
 */
std::string& stringVAppendf(std::string* out, const char* format, va_list ap);

/**
 * Backslashify a string.
 *
 * That is, replace non-printable characters
 * with C-style (but NOT C compliant) "\xHH" encoding.  If hex_style
 * is false, then shorthand notations like "\0" will be used instead
 * of "\x00" for the most common backslash cases.
 *
 * There are two forms, one returning the input string, and one
 * creating output in the specified output string.
 *
 * This is mainly intended for printing to a terminal, so it is not
 * particularly optimized.
 *
 * Do *not* use this in situations where you expect to be able to feed
 * the string to a C or C++ compiler, as there are nuances with how C
 * parses such strings that lead to failures.  This is for display
 * purposed only.  If you want a string you can embed for use in C or
 * C++, use cEscape instead.  This function is for display purposes
 * only.
 */
template <class OutputString>
void backslashify(
    folly::StringPiece input, OutputString& output, bool hex_style = false);

template <class OutputString = std::string>
OutputString backslashify(StringPiece input, bool hex_style = false) {
  OutputString output;
  backslashify(input, output, hex_style);
  return output;
}

/**
 * Take a string and "humanify" it -- that is, make it look better.
 *
 * Since "better" is subjective, caveat emptor.  The basic approach is
 * to count the number of unprintable characters.  If there are none,
 * then the output is the input.  If there are relatively few, or if
 * there is a long "enough" prefix of printable characters, use
 * backslashify.  If it is mostly binary, then simply hex encode.
 *
 * This is an attempt to make a computer smart, and so likely is wrong
 * most of the time.
 */
template <class String1, class String2>
void humanify(const String1& input, String2& output);

template <class String>
String humanify(const String& input) {
  String output;
  humanify(input, output);
  return output;
}

/**
 * Convert input to hexadecimal representation.
 *
 * Same functionality as Python's binascii.hexlify.  Returns true
 * on successful conversion.
 *
 * If append_output is true, append data to the output rather than
 * replace it.
 */
template <class InputString, class OutputString>
bool hexlify(
    const InputString& input, OutputString& output, bool append = false);

template <class OutputString, size_t Extent>
bool hexlify(
    std::span<const uint8_t, Extent> input,
    OutputString& output,
    bool append = false) {
  return hexlify(ByteRange{input.data(), input.size()}, output, append);
}

template <class OutputString = std::string>
OutputString hexlify(ByteRange input) {
  OutputString output;
  if (!hexlify(input, output)) {
    // hexlify() currently always returns true, so this can't really happen
    throw_exception<std::runtime_error>("hexlify failed");
  }
  return output;
}

template <class OutputString = std::string, size_t Extent>
OutputString hexlify(std::span<const uint8_t, Extent> input) {
  return hexlify<OutputString>(ByteRange{input.data(), input.size()});
}

template <class OutputString = std::string>
OutputString hexlify(StringPiece input) {
  return hexlify<OutputString>(ByteRange{input});
}

/**
 * Get binary data from hexadecimal representation.
 *
 * Same functionality as Python's binascii.unhexlify.  Returns true
 * on successful conversion.
 */
template <class InputString, class OutputString>
bool unhexlify(const InputString& input, OutputString& output);

template <class OutputString = std::string>
OutputString unhexlify(StringPiece input) {
  OutputString output;
  if (!unhexlify(input, output)) {
    // unhexlify() fails if the input has non-hexidecimal characters,
    // or if it doesn't consist of a whole number of bytes
    throw_exception<std::domain_error>("unhexlify() called with non-hex input");
  }
  return output;
}

enum PrettyType {
  PRETTY_TIME,
  PRETTY_TIME_HMS,

  PRETTY_BYTES_METRIC,
  PRETTY_BYTES_BINARY,
  PRETTY_BYTES = PRETTY_BYTES_BINARY,
  PRETTY_BYTES_BINARY_IEC,
  PRETTY_BYTES_IEC = PRETTY_BYTES_BINARY_IEC,

  PRETTY_UNITS_METRIC,
  PRETTY_UNITS_BINARY,
  PRETTY_UNITS_BINARY_IEC,

  PRETTY_SI,

  PRETTY_BITS_METRIC,
  PRETTY_BITS = PRETTY_BITS_METRIC,

  PRETTY_NUM_TYPES,
};

/**
 * Pretty printer for numbers with units.
 *
 * A pretty-printer for numbers that appends suffixes of units of the
 * given type.  It prints 4 sig-figs of value with the most
 * appropriate unit.
 *
 * If `addSpace' is true, we put a space between the units suffix and
 * the value.
 *
 * Current types are:
 *     PRETTY_TIME         - s, ms, us, ns, etc.
 *     PRETTY_TIME_HMS     - h, m, s, ms, us, ns, etc.
 *     PRETTY_BYTES_METRIC - kB, MB, GB, etc (goes up by 10^3 = 1000 each time)
 *     PRETTY_BYTES        - kB, MB, GB, etc (goes up by 2^10 = 1024 each time)
 *     PRETTY_BYTES_IEC    - KiB, MiB, GiB, etc
 *     PRETTY_UNITS_METRIC - k, M, G, etc (goes up by 10^3 = 1000 each time)
 *     PRETTY_UNITS_BINARY - k, M, G, etc (goes up by 2^10 = 1024 each time)
 *     PRETTY_UNITS_BINARY_IEC - Ki, Mi, Gi, etc
 *     PRETTY_SI           - full SI metric prefixes from yocto to Yotta
 *                           http://en.wikipedia.org/wiki/Metric_prefix
 *     PRETTY_BITS_METRIC  - kb, Mb, Gb, etc (goes up by 10^3 = 1000 each time)
 *                           Useful for network bandwidth (e.g., 1 Gbps)
 *     PRETTY_BITS         - alias for PRETTY_BITS_METRIC
 *
 */
std::string prettyPrint(double val, PrettyType, bool addSpace = true);

/**
 * @overloadbrief Reverse prettyPrint.
 *
 * This utility converts StringPiece in pretty format (look above) to double,
 * with progress information. Alters the  StringPiece parameter
 * to get rid of the already-parsed characters.
 * Expects string in form <floating point number> {space}* [<suffix>]
 * If string is not in correct format, utility finds longest valid prefix and
 * if there at least one, returns double value based on that prefix and
 * modifies string to what is left after parsing. Throws and std::range_error
 * exception if there is no correct parse.
 * Examples(for PRETTY_UNITS_METRIC):
 * '10M' => 10 000 000
 * '10 M' => 10 000 000
 * '10' => 10
 * '10 Mx' => 10 000 000, prettyString == "x"
 * 'abc' => throws std::range_error
 */
double prettyToDouble(
    folly::StringPiece* const prettyString, const PrettyType type);

/**
 * Same as prettyToDouble(folly::StringPiece*, PrettyType), but
 * expects whole string to be correctly parseable. Throws std::range_error
 * otherwise
 */
double prettyToDouble(folly::StringPiece prettyString, const PrettyType type);

/**
 * @overloadbrief Write a hex dump of size bytes starting at ptr to out.
 *
 * The hex dump is formatted as follows:
 *
 * for the string "abcdefghijklmnopqrstuvwxyz\x02"
00000000  61 62 63 64 65 66 67 68  69 6a 6b 6c 6d 6e 6f 70  |abcdefghijklmnop|
00000010  71 72 73 74 75 76 77 78  79 7a 02                 |qrstuvwxyz.     |
 *
 * that is, we write 16 bytes per line, both as hex bytes and as printable
 * characters.  Non-printable characters are replaced with '.'
 * Lines are written to out one by one (one StringPiece at a time) without
 * delimiters.
 */
template <class OutIt>
void hexDump(const void* ptr, size_t size, OutIt out);

/**
 * Return the hex dump of size bytes starting at ptr as a string.
 */
std::string hexDump(const void* ptr, size_t size);

/**
 * Pretty print an errno.
 *
 * Return a string containing the description of the given errno value.
 * Takes care not to overwrite the actual system errno, so calling
 * errnoStr(errno) is valid.
 */
std::string errnoStr(int err);

template <typename T, std::size_t M, typename P>
class small_vector;

template <typename T, typename Allocator>
class fbvector;

namespace detail {

// We don't use SimdSplitByCharIsDefinedFor because
// we would like the user to get an error where they could use SIMD
// implementation but didn't use quite correct parameters.
template <typename>
struct IsSplitSupportedContainer : std::false_type {};

template <typename T>
using HasSimdSplitCompatibleValueType =
    std::is_convertible<typename T::value_type, folly::StringPiece>;

template <typename T, typename A>
struct IsSplitSupportedContainer<std::vector<T, A>> : std::true_type {};

template <typename T, typename A>
struct IsSplitSupportedContainer<fbvector<T, A>> : std::true_type {};

template <typename T, std::size_t M, typename P>
struct IsSplitSupportedContainer<small_vector<T, M, P>> : std::true_type {};

template <typename>
struct IsSimdSupportedDelim : std::false_type {};

template <>
struct IsSimdSupportedDelim<char> : std::true_type {};

} // namespace detail

/**
 * Split a string into a list of tokens by delimiter.
 *
 * The split interface here supports different output types, selected
 * at compile time: StringPiece, fbstring, or std::string.  If you are
 * using a vector to hold the output, it detects the type based on
 * what your vector contains.  If the output vector is not empty, split
 * will append to the end of the vector.
 *
 * You can also use splitTo() to write the output to an arbitrary
 * OutputIterator (e.g. std::inserter() on a std::set<>), in which
 * case you have to tell the function the type.  (Rationale:
 * OutputIterators don't have a value_type, so we can't detect the
 * type in splitTo without being told.)
 *
 * Examples:
 *
 *   std::vector<folly::StringPiece> v;
 *   folly::split(':', "asd:bsd", v);
 *
 *   folly::small_vector<folly::StringPiece, 3> v;
 *   folly::split(':', "asd:bsd:csd", v)
 *
 *   std::set<StringPiece> s;
 *   folly::splitTo<StringPiece>("::", "asd::bsd::asd::csd",
 *    std::inserter(s, s.begin()));
 *
 * Split also takes a flag (ignoreEmpty) that indicates whether adjacent
 * delimiters should be treated as one single separator (ignoring empty tokens)
 * or not (generating empty tokens).
 */

template <class Delim, class String, class OutputType>
  requires(
      detail::IsSimdSupportedDelim<Delim>::value &&
      detail::HasSimdSplitCompatibleValueType<OutputType>::value &&
      detail::IsSplitSupportedContainer<OutputType>::value)
FOLLY_ALWAYS_INLINE void split(
    const Delim& delimiter,
    const String& input,
    OutputType& out,
    const bool ignoreEmpty = false) {
  return detail::simdSplitByChar(delimiter, input, out, ignoreEmpty);
}

template <class Delim, class String, class OutputType>
  requires(
      (!detail::IsSimdSupportedDelim<Delim>::value ||
       !detail::HasSimdSplitCompatibleValueType<OutputType>::value) &&
      detail::IsSplitSupportedContainer<OutputType>::value)
void split(
    const Delim& delimiter,
    const String& input,
    OutputType& out,
    const bool ignoreEmpty = false);

/**
 * Split a string into a list of tokens by delimiter with options.
 *
 * Same as split() above but with additional options to control behavior.
 * The SplitOptions allow enabling preallocation which can improve performance
 * when splitting large strings with many expected tokens.
 */
template <class Delim, class String, class OutputType>
  requires detail::IsSplitSupportedContainer<OutputType>::value
void split(
    const Delim& delimiter,
    const String& input,
    OutputType& out,
    const SplitOptions& options);

/**
 * split, to an output iterator
 */
template <
    class OutputValueType,
    class Delim,
    class String,
    class OutputIterator>
void splitTo(
    const Delim& delimiter,
    const String& input,
    OutputIterator out,
    const bool ignoreEmpty = false);

namespace detail {
template <typename Void, typename OutputType>
struct IsConvertible : std::false_type {};

template <>
struct IsConvertible<void, decltype(std::ignore)> : std::true_type {};

template <typename OutputType>
struct IsConvertible<
    std::void_t<decltype(parseTo(StringPiece{}, std::declval<OutputType&>()))>,
    OutputType> : std::true_type {};
} // namespace detail
template <typename OutputType>
struct IsConvertible : detail::IsConvertible<void, OutputType> {};

/**
 * Split a string into a fixed number of string pieces and/or numeric types
 * by delimiter. Conversions are supported for any type which folly:to<> can
 * target, including all overloads of parseTo(). Returns 'true' if the fields
 * were all successfully populated.  Returns 'false' if there were too few
 * fields in the input, or too many fields if exact=true.  Casting exceptions
 * will not be caught.
 *
 * Examples:
 *
 *  folly::StringPiece name, key, value;
 *  if (folly::split('\t', line, name, key, value))
 *    ...
 *
 *  folly::StringPiece name;
 *  double value;
 *  int id;
 *  if (folly::split('\t', line, name, value, id))
 *    ...
 *
 * The 'exact' template parameter specifies how the function behaves when too
 * many fields are present in the input string. When 'exact' is set to its
 * default value of 'true', a call to split will fail if the number of fields in
 * the input string does not exactly match the number of output parameters
 * passed. If 'exact' is overridden to 'false', all remaining fields will be
 * stored, unsplit, in the last field, as shown below:
 *
 *  folly::StringPiece x, y.
 *  if (folly::split<false>(':', "a:b:c", x, y))
 *    assert(x == "a" && y == "b:c");
 *
 * Note that this will likely not work if the last field's target is of numeric
 * type, in which case folly::to<> will throw an exception.
 */
template <bool exact = true, class Delim, class... OutputTypes>
  requires(
      StrictConjunction<IsConvertible<OutputTypes>...>::value &&
      sizeof...(OutputTypes) >= 1)
bool split(const Delim& delimiter, StringPiece input, OutputTypes&... outputs);

// Error type for trySplitTo(), below.
struct SubstringConversionCode {
  StringPiece substring;
  ConversionCode code;
  bool operator==(const SubstringConversionCode& other) const;
};

/**
 * Try to split a string into a fixed number of fields by delimiter, using
 * folly::tryTo<> for conversions. types by delimiter.
 * - On success, all output values will be initialized and the 'Unit{}' value is
 *   returned. Arguments are assigned in reverse order.
 * - On failure, the first failing 'ConversionCode' is returned with its
 *   associated substring in a 'SubstringConversionCode'.
 * - String splitting is performed prior to each conversion; field values will
 *   not contain the delimiter.
 * - All custom error codes are mapped to ConversionCode::CUSTOM.
 *
 * Examples:
 *
 *  folly::StringPiece name, key, value;
 *  if (folly::trySplitTo(line, '\t',  name, key, value))
 *    ...
 *
 *  folly::StringPiece name;
 *  double value;
 *  int id;
 *  if (folly::trySplitTo(line, '\t', name, value, id))
 *    ...
 *
 */
template <class Delim, class... OutputTypes>
  requires StrictConjunction<IsConvertible<OutputTypes>...>::value
Expected<Unit, SubstringConversionCode> trySplitTo(
    StringPiece input, const Delim& delimiter, OutputTypes&... outputs);

/**
 * Join list of tokens.
 *
 * Stores a string representation of tokens in the same order with
 * delimiter between each element.
 */
template <class Delim, class Iterator, class String>
void join(const Delim& delimiter, Iterator begin, Iterator end, String& output);

template <class Delim, class Container, class String>
void join(const Delim& delimiter, const Container& container, String& output) {
  join(delimiter, container.begin(), container.end(), output);
}

template <class Delim, class Value, class String>
void join(
    const Delim& delimiter,
    const std::initializer_list<Value>& values,
    String& output) {
  join(delimiter, values.begin(), values.end(), output);
}

template <class Delim, class Container>
std::string join(const Delim& delimiter, const Container& container) {
  std::string output;
  join(delimiter, container.begin(), container.end(), output);
  return output;
}

template <class Delim, class Value>
std::string join(
    const Delim& delimiter, const std::initializer_list<Value>& values) {
  std::string output;
  join(delimiter, values.begin(), values.end(), output);
  return output;
}

template <
    class Delim,
    class Iterator,
    typename std::enable_if<std::is_base_of<
        std::forward_iterator_tag,
        typename std::iterator_traits<Iterator>::iterator_category>::value>::
        type* = nullptr>
std::string join(const Delim& delimiter, Iterator begin, Iterator end) {
  std::string output;
  join(delimiter, begin, end, output);
  return output;
}

/**
 * Remove leading whitespace.
 *
 * Returns a subpiece with all whitespace removed from the front of @sp.
 * Whitespace means any of [' ', '\n', '\r', '\t'].
 */
StringPiece ltrimWhitespace(StringPiece sp);

/**
 * Remove trailing whitespace.
 *
 * Returns a subpiece with all whitespace removed from the back of @sp.
 * Whitespace means any of [' ', '\n', '\r', '\t'].
 */
StringPiece rtrimWhitespace(StringPiece sp);

/**
 * Remove leading and trailing whitespace.
 *
 * Returns a subpiece with all whitespace removed from the back and front of
 * @sp. Whitespace means any of [' ', '\n', '\r', '\t'].
 */
inline StringPiece trimWhitespace(StringPiece sp) {
  return ltrimWhitespace(rtrimWhitespace(sp));
}

/**
 * DEPRECATED: Use ltrimWhitespace instead
 *
 * Returns a subpiece with all whitespace removed from the front of @sp.
 * Whitespace means any of [' ', '\n', '\r', '\t'].
 */
inline StringPiece skipWhitespace(StringPiece sp) {
  return ltrimWhitespace(sp);
}

/**
 * Specify characters to ltrim.
 *
 * Returns a subpiece with all characters the provided @toTrim returns true
 * for removed from the front of @sp.
 */
template <typename ToTrim>
StringPiece ltrim(StringPiece sp, ToTrim toTrim) {
  while (!sp.empty() && toTrim(sp.front())) {
    sp.pop_front();
  }

  return sp;
}

/**
 * Specify characters to rtrim.
 *
 * Returns a subpiece with all characters the provided @toTrim returns true
 * for removed from the back of @sp.
 */
template <typename ToTrim>
StringPiece rtrim(StringPiece sp, ToTrim toTrim) {
  while (!sp.empty() && toTrim(sp.back())) {
    sp.pop_back();
  }

  return sp;
}

/**
 * Specify characters to trim.
 *
 * Returns a subpiece with all characters the provided @toTrim returns true
 * for removed from the back and front of @sp.
 */
template <typename ToTrim>
StringPiece trim(StringPiece sp, ToTrim toTrim) {
  return ltrim(rtrim(sp, std::ref(toTrim)), std::ref(toTrim));
}

/**
 * De-indent a string.
 *
 * Strips the leading and the trailing whitespace-only lines. Then looks for
 * the least indented non-whitespace-only line and removes its amount of
 * leading whitespace from every line. Assumes leading whitespace is either all
 * spaces or all tabs.
 *
 * Purpose: including a multiline string literal in source code, indented to
 * the level expected from context.
 */
std::string stripLeftMargin(std::string s);

/**
 * Convert ascii to lowercase, in-place.
 *
 * Leaves all other characters unchanged, including those with the 0x80
 * bit set.
 * @param str String to convert
 * @param length Length of str, in bytes
 */
void toLowerAscii(char* str, size_t length);

inline void toLowerAscii(MutableStringPiece str) {
  toLowerAscii(str.begin(), str.size());
}

inline void toLowerAscii(std::string& str) {
  // str[0] is legal also if the string is empty.
  toLowerAscii(&str[0], str.size());
}

/**
 * Convert ascii to uppercase, in-place.
 *
 * Leaves all other characters unchanged, including those with the 0x80
 * bit set.
 * @param str String to convert
 * @param length Length of str, in bytes
 */
void toUpperAscii(char* str, size_t length);

inline void toUpperAscii(MutableStringPiece str) {
  toUpperAscii(str.begin(), str.size());
}

inline void toUpperAscii(std::string& str) {
  // str[0] is legal also if the string is empty.
  toUpperAscii(&str[0], str.size());
}

/**
 * Returns if string contains std::isspace or std::iscntrl characters.
 **/
inline bool hasSpaceOrCntrlSymbols(folly::StringPiece s) {
  return detail::simdHasSpaceOrCntrlSymbols(s);
}

struct format_string_for_each_named_arg_fn {
  struct options {
    bool numeric_args_as_named = false;

    options& set_numeric_args_as_named(bool value) noexcept {
      numeric_args_as_named = value;
      return *this;
    }
  };

  template <typename C, typename CT, typename Fn>
  constexpr void operator()(std::basic_string_view<C, CT> str, Fn fn) const
      noexcept(noexcept(fn(str))) {
    return operator()(options{}, str, std::ref(fn));
  }

  template <typename C, typename CT, typename Fn>
  constexpr void operator()(
      options const& opts, std::basic_string_view<C, CT> str, Fn fn) const
      noexcept(noexcept(fn(str))) {
    using view = std::basic_string_view<C, CT>;
    while (true) {
      auto const pos = str.find('{');
      auto const beg = pos == view::npos ? str.size() : pos + 1;
      if (beg == str.size()) {
        return; // completed
      }
      if (str[beg] == '{') {
        str = str.substr(beg + 1);
        continue; // escaped
      }
      auto const end = std::min(str.find('}', pos), str.find(':', pos));
      if (end == view::npos) {
        return; // malformed
      }
      auto const arg = str.substr(beg, end - beg);
      auto const c = arg.empty() ? 0 : arg[0];
      if (c && (opts.numeric_args_as_named || !(c >= '0' && c <= '9'))) {
        fn(arg);
      }
      str = str.substr(end);
    }
  }
};

inline constexpr format_string_for_each_named_arg_fn
    format_string_for_each_named_arg{};

using format_string_for_each_named_arg_options =
    format_string_for_each_named_arg_fn::options;

} // namespace folly

#include <folly/String-inl.h>
