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
// Docs: https://fburl.com/fbcref_json
//

/**
 * Serialize and deserialize folly::dynamic values as JSON.
 *
 * Basic JSON type system:
 *
 *     Value  : String | Bool | Null | Object | Array | Number
 *     String : UTF-8 sequence
 *     Object : (String, Value) pairs, with unique String keys
 *     Array  : ordered list of Values
 *     Null   : null
 *     Bool   : true | false
 *     Number : (representation unspecified)
 *
 * For more information see http://json.org or look up RFC 4627.
 *
 * If your dynamic has anything illegal with regard to this type
 * system, the serializer will throw.
 *
 * @file json.h
 */

#pragma once

#include <array>
#include <iosfwd>
#include <string>
#include <unordered_map>

#include <folly/Function.h>
#include <folly/Range.h>
#include <folly/json/dynamic.h>

namespace folly {

//////////////////////////////////////////////////////////////////////

namespace json {

// Specifies how to format floating-point values in serialized JSON output.
// Each enum value maps to a fixed fmt format string at the printer dispatch
// site, so there is no runtime fmt-spec parsing on the serialization hot path.
enum class FloatFormat {
  // Shortest decimal that round-trips back to the exact same IEEE-754 double.
  // Equivalent to `fmt::format("{}", x)`.
  SHORTEST,
  // Like SHORTEST, but always emits a decimal point and at least one fractional
  // digit (e.g. `123` becomes `123.0`, `3e-06` becomes `3.0e-06`). Required by
  // JSON wire formats that need to disambiguate floats from integers in text.
  // Note: not equivalent to fmt's `{:#}` alternate form, which emits a bare
  // trailing dot for integer-mantissa values in scientific notation
  // (e.g. `"3.e-06"`), violating RFC 8259 §6.
  SHORTEST_TRAILING_DOT_ZERO,
  // Like SHORTEST, but uses single-precision rounding: finds the shortest
  // decimal that round-trips to the same IEEE-754 float. Values originally
  // stored as 32-bit floats benefit from this because the extra digits in their
  // double representation are precision noise (e.g. 4.099999904632568 → "4.1").
  // Equivalent to `fmt::format("{}", static_cast<float>(x))`.
  SHORTEST_SINGLE,
  // Like SHORTEST_SINGLE, but always emits a decimal point and at least one
  // fractional digit (e.g. `123` becomes `123.0`, `3e-06` becomes `3.0e-06`).
  // See the SHORTEST_TRAILING_DOT_ZERO note about fmt's `{:#}`.
  SHORTEST_SINGLE_TRAILING_DOT_ZERO,
  // Fixed-point notation with `double_num_digits` digits after the decimal
  // point. Equivalent to `fmt::format("{:.{}f}", x, double_num_digits)`.
  FIXED,
  // General-precision notation (`%g` semantics) with `double_num_digits`
  // significant digits. Strips trailing zeros. Equivalent to
  // `fmt::format("{:.{}g}", x, double_num_digits)`.
  GENERAL,
};

struct serialization_opts {
  // If true, keys in an object can be non-strings.  (In strict
  // JSON, object keys must be strings.)  This is used by dynamic's
  // operator<<.
  bool allow_non_string_keys{false};

  // If true, integer keys are allowed irrespective of 'allow_non_string_keys'
  // in parsing, and are converted to strings in serialization. Other types are
  // accepted accoring to 'allow_non_string_keys'. If set, validate_keys is
  // enabled. Any key sorting will be applied on the stored integers, if present
  // in the input object.
  bool convert_int_keys{false};

  /*
   * If true, refuse to serialize 64-bit numbers that cannot be
   * precisely represented by fit a double---instead, throws an
   * exception if the document contains this.
   */
  bool javascript_safe{false};

  // If true, the serialized json will contain space and newlines to
  // try to be minimally "pretty".
  bool pretty_formatting{false};

  // The number of spaces to indent by when pretty_formatting is enabled.
  unsigned int pretty_formatting_indent_width{2};

  // If true, non-ASCII utf8 characters would be encoded as \uXXXX:
  // - if the code point is in [U+0000..U+FFFF] => encode as a single \uXXXX
  // - if the code point is > U+FFFF => encode as 2 UTF-16 surrogate pairs.
  bool encode_non_ascii{false};

  // Check that strings are valid utf8
  bool validate_utf8{false};

  // Check that keys are distinct
  bool validate_keys{false};

  // Allow trailing comma in lists of values / items
  bool allow_trailing_comma{false};

  // Sort keys of all objects before printing out (potentially slow)
  // using dynamic::operator<.
  // Has no effect if sort_keys_by is set.
  bool sort_keys{false};

  // Sort keys of all objects before printing out (potentially slow)
  // using the provided less functor.
  Function<bool(dynamic const&, dynamic const&) const> sort_keys_by{};

  // Replace invalid utf8 characters with U+FFFD and continue
  bool skip_invalid_utf8{false};

  // true to allow NaN or INF values
  bool allow_nan_inf{false};

  FloatFormat float_format{FloatFormat::SHORTEST};

  unsigned int double_num_digits{0}; // used by FIXED and GENERAL formats

  // Fallback to double when a value that looks like integer is too big to
  // fit in an int64_t. Can result in loss a of precision.
  bool double_fallback{false};

  // Do not parse numbers. Instead, store them as strings and leave the
  // conversion up to the user.
  bool parse_numbers_as_strings{false};

  // Recursion limit when parsing.
  unsigned int recursion_limit{100};

  // Bitmap representing ASCII characters to escape with unicode
  // representations. The least significant bit of the first in the pair is
  // ASCII value 0; the most significant bit of the second in the pair is ASCII
  // value 127. Some specific characters in this range are always escaped
  // regardless of the bitmask - namely characters less than 0x20, \, and ".
  std::array<uint64_t, 2> extra_ascii_to_escape_bitmap{};

  // Allow json5 string. This implies allowing trailing comma, nan, inf, etc.
  bool allow_json5_experimental{false};
};

/**
 * Create bitmap for serialization_opts.extra_ascii_to_escape_bitmap.
 *
 * Generates a bitmap with bits set for each of the ASCII characters provided
 * for use in the serialization_opts extra_ascii_to_escape_bitmap option. If any
 * characters are not valid ASCII, they are ignored.
 */
std::array<uint64_t, 2> buildExtraAsciiToEscapeBitmap(StringPiece chars);

/**
 * Serialize dynamic to json-string, with options.
 *
 * Main JSON serialization routine taking folly::dynamic parameters.
 * For the most common use cases there are simpler functions in the
 * main folly namespace.
 */
std::string serialize(dynamic const&, serialization_opts const&);

/**
 * Escape a string so that it is legal to print it in JSON text.
 *
 * Append the result to out.
 */
void escapeString(
    StringPiece input, std::string& out, const serialization_opts& opts);

/**
 * Strip all C99-like comments (i.e. // and / * ... * /)
 */
std::string stripComments(StringPiece jsonC);

// Parse error can be thrown when deserializing json (ie., converting string
// into json).
class FOLLY_EXPORT parse_error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Print error can be thrown when serializing json (ie., converting json into
// string).
class FOLLY_EXPORT print_error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// may be extened in future to include offset, col, etc.
struct parse_location {
  uint32_t line{}; // 0-indexed
};

// may be extended in future to include end location
struct parse_range {
  parse_location begin;
};

struct parse_metadata {
  parse_range key_range;
  parse_range value_range;
};

using metadata_map = std::unordered_map<dynamic const*, parse_metadata>;

} // namespace json

//////////////////////////////////////////////////////////////////////

/**
 * Parse a json blob out of a range and produce a dynamic representing
 * it.
 */
dynamic parseJson(StringPiece, json::serialization_opts const&);
dynamic parseJson(StringPiece);

[[deprecated("This is an experimental feature. Do not use in production.")]]
dynamic parseJson5(StringPiece);

dynamic parseJsonWithMetadata(StringPiece range, json::metadata_map* map);
dynamic parseJsonWithMetadata(
    StringPiece range,
    json::serialization_opts const& opts,
    json::metadata_map* map);

/**
 * Serialize a dynamic into a json string.
 */
std::string toJson(dynamic const&);

/**
 * Serialize a dynamic into a json string with indentation.
 * Note that the keys of all objects will be sorted.
 */
std::string toPrettyJson(dynamic const&);

/**
 * Printer for GTest.
 *
 * Uppercase name to fill GTest's API, which calls this method through ADL.
 */
void PrintTo(const dynamic&, std::ostream*);
//////////////////////////////////////////////////////////////////////

} // namespace folly
