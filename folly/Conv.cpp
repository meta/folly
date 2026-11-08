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

#include <folly/Conv.h>

#include <algorithm>
#include <array>
#include <istream>

#include <fmt/format.h>
#include <folly/lang/SafeAssert.h>

// fast_float parses several times faster than any standard library; define
// FOLLY_USE_FAST_FLOAT to 0 to force the std::from_chars fallback.
#if !defined(FOLLY_USE_FAST_FLOAT)
#if __has_include(<fast_float/fast_float.h>)
#define FOLLY_USE_FAST_FLOAT 1
#else
#define FOLLY_USE_FAST_FLOAT 0
#endif
#endif

#include <charconv>
#include <cstdint>
#include <limits>
#include <type_traits>

#if FOLLY_USE_FAST_FLOAT
#include <fast_float/fast_float.h>
#endif

namespace folly {
namespace detail {

namespace {

/**
 * Finds the first non-digit in a string. The number of digits
 * searched depends on the precision of the Tgt integral. Assumes the
 * string starts with NO whitespace and NO sign.
 *
 * The semantics of the routine is:
 *   for (;; ++b) {
 *     if (b >= e || !isdigit(*b)) return b;
 *   }
 *
 *  Complete unrolling marks bottom-line (i.e. entire conversion)
 *  improvements of 20%.
 */
inline const char* findFirstNonDigit(const char* b, const char* e) {
  for (; b < e; ++b) {
    auto const c = static_cast<unsigned>(*b) - '0';
    if (c >= 10) {
      break;
    }
  }
  return b;
}

// Maximum value of number when represented as a string
template <class T>
struct MaxString {
  static const char* const value;
};

template <>
const char* const MaxString<uint8_t>::value = "255";
template <>
const char* const MaxString<uint16_t>::value = "65535";
template <>
const char* const MaxString<uint32_t>::value = "4294967295";
#if __SIZEOF_LONG__ == 4
template <>
const char* const MaxString<unsigned long>::value = "4294967295";
#else
template <>
const char* const MaxString<unsigned long>::value = "18446744073709551615";
#endif
static_assert(
    sizeof(unsigned long) >= 4,
    "Wrong value for MaxString<unsigned long>::value,"
    " please update.");
template <>
const char* const MaxString<unsigned long long>::value = "18446744073709551615";
static_assert(
    sizeof(unsigned long long) >= 8,
    "Wrong value for MaxString<unsigned long long>::value"
    ", please update.");

#if FOLLY_HAVE_INT128_T
template <>
const char* const MaxString<__uint128_t>::value =
    "340282366920938463463374607431768211455";
#endif

/*
 * Lookup tables that converts from a decimal character value to an integral
 * binary value, shifted by a decimal "shift" multiplier.
 * For all character values in the range '0'..'9', the table at those
 * index locations returns the actual decimal value shifted by the multiplier.
 * For all other values, the lookup table returns an invalid OOR value.
 */
// Out-of-range flag value, larger than the largest value that can fit in
// four decimal bytes (9999), but four of these added up together should
// still not overflow uint16_t.
constexpr int32_t OOR = 10000;

alignas(16) constexpr uint16_t shift1[] = {
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 0-9
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  10
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  20
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  30
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, 0,   1, //  40
    2,   3,   4,   5,   6,   7,   8,   9,   OOR, OOR,
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  60
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  70
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  80
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  90
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 100
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 110
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 120
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 130
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 140
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 150
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 160
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 170
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 180
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 190
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 200
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 210
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 220
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 230
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 240
    OOR, OOR, OOR, OOR, OOR, OOR // 250
};

alignas(16) constexpr uint16_t shift10[] = {
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 0-9
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  10
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  20
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  30
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, 0,   10, //  40
    20,  30,  40,  50,  60,  70,  80,  90,  OOR, OOR,
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  60
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  70
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  80
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  90
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 100
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 110
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 120
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 130
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 140
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 150
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 160
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 170
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 180
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 190
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 200
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 210
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 220
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 230
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 240
    OOR, OOR, OOR, OOR, OOR, OOR // 250
};

alignas(16) constexpr uint16_t shift100[] = {
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 0-9
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  10
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  20
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  30
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, 0,   100, //  40
    200, 300, 400, 500, 600, 700, 800, 900, OOR, OOR,
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  60
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  70
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  80
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, //  90
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 100
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 110
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 120
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 130
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 140
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 150
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 160
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 170
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 180
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 190
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 200
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 210
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 220
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 230
    OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, OOR, // 240
    OOR, OOR, OOR, OOR, OOR, OOR // 250
};

alignas(16) constexpr uint16_t shift1000[] = {
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 0-9
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, //  10
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, //  20
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, //  30
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  0,   1000, //  40
    2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, OOR, OOR,
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, //  60
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, //  70
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, //  80
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, //  90
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 100
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 110
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 120
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 130
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 140
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 150
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 160
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 170
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 180
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 190
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 200
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 210
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 220
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 230
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR,  OOR, OOR, // 240
    OOR,  OOR,  OOR,  OOR,  OOR,  OOR // 250
};

struct ErrorString {
  const char* string;
  bool quote;
};

// Keep this in sync with ConversionCode in Conv.h
constexpr const std::array<
    ErrorString,
    static_cast<std::size_t>(ConversionCode::NUM_ERROR_CODES)>
    kErrorStrings{{
        // SUCCESS
        {"Success", true},
        // EMPTY_INPUT_STRING
        {"Empty input string", true},
        // NO_DIGITS
        {"No digits found in input string", true},
        // BOOL_OVERFLOW
        {"Integer overflow when parsing bool (must be 0 or 1)", true},
        // BOOL_INVALID_VALUE
        {"Invalid value for bool", true},
        // NON_DIGIT_CHAR
        {"Non-digit character found", true},
        // INVALID_LEADING_CHAR
        {"Invalid leading character", true},
        // POSITIVE_OVERFLOW
        {"Overflow during conversion", true},
        // NEGATIVE_OVERFLOW
        {"Negative overflow during conversion", true},
        // STRING_TO_FLOAT_ERROR
        {"Unable to convert string to floating point value", true},
        // NON_WHITESPACE_AFTER_END
        {"Non-whitespace character found after end of conversion", true},
        // ARITH_POSITIVE_OVERFLOW
        {"Overflow during arithmetic conversion", false},
        // ARITH_NEGATIVE_OVERFLOW
        {"Negative overflow during arithmetic conversion", false},
        // ARITH_LOSS_OF_PRECISION
        {"Loss of precision during arithmetic conversion", false},
        // SPLIT_ERROR,
        {"Unexpected number of fields resulting from a split", true},
        // CUSTOM,
        {"Custom conversion failed", true},
    }};

// Check if ASCII is really ASCII
using IsAscii =
    std::bool_constant<'A' == 65 && 'Z' == 90 && 'a' == 97 && 'z' == 122>;

// The code in this file that uses tolower() really only cares about
// 7-bit ASCII characters, so we can take a nice shortcut here.
inline char tolower_ascii(char in) {
  return IsAscii::value ? in | 0x20 : char(std::tolower(in));
}

inline bool bool_str_cmp(const char** b, size_t len, const char* value) {
  // Can't use strncasecmp, since we want to ensure that the full value matches
  const char* p = *b;
  const char* e = *b + len;
  const char* v = value;
  while (*v != '\0') {
    if (p == e || tolower_ascii(*p) != *v) { // value is already lowercase
      return false;
    }
    ++p;
    ++v;
  }

  *b = p;
  return true;
}

} // namespace

Expected<bool, ConversionCode> str_to_bool(std::string_view* src) noexcept {
  auto b = src->data(), e = src->data() + src->size();
  for (;; ++b) {
    if (b >= e) {
      return makeUnexpected(ConversionCode::EMPTY_INPUT_STRING);
    }
    if ((*b < '\t' || *b > '\r') && *b != ' ') {
      break;
    }
  }

  bool result;
  auto len = size_t(e - b);
  switch (*b) {
    case '0':
    case '1': {
      result = false;
      for (; b < e && isdigit(*b); ++b) {
        if (result || (*b != '0' && *b != '1')) {
          return makeUnexpected(ConversionCode::BOOL_OVERFLOW);
        }
        result = (*b == '1');
      }
      break;
    }
    case 'y':
    case 'Y':
      result = true;
      if (!bool_str_cmp(&b, len, "yes")) {
        ++b; // accept the single 'y' character
      }
      break;
    case 'n':
    case 'N':
      result = false;
      if (!bool_str_cmp(&b, len, "no")) {
        ++b;
      }
      break;
    case 't':
    case 'T':
      result = true;
      if (!bool_str_cmp(&b, len, "true")) {
        ++b;
      }
      break;
    case 'f':
    case 'F':
      result = false;
      if (!bool_str_cmp(&b, len, "false")) {
        ++b;
      }
      break;
    case 'o':
    case 'O':
      if (bool_str_cmp(&b, len, "on")) {
        result = true;
      } else if (bool_str_cmp(&b, len, "off")) {
        result = false;
      } else {
        return makeUnexpected(ConversionCode::BOOL_INVALID_VALUE);
      }
      break;
    default:
      return makeUnexpected(ConversionCode::BOOL_INVALID_VALUE);
  }

  src->remove_prefix(size_t(b - src->data()));

  return result;
}

Expected<bool, ConversionCode> str_to_bool(StringPiece* src) noexcept {
  auto view = std::string_view(src->data(), src->size());
  auto result = str_to_bool(&view);
  src->assign(view.data(), src->end());
  return result;
}

namespace {

const char* skip_leading_space(const char* b, const char* e) noexcept {
  return std::find_if_not(b, e, [](char c) {
    return (c >= '\t' && c <= '\r') || c == ' ';
  });
}

#if FOLLY_HAVE_STD_FLOAT_FROM_CHARS

/// Whether the number in `[b, e)`, known to be out of range, is the tiny one
/// rather than the huge one. With its magnitude written as `0.d... * 10^k` and
/// a nonzero `d`, k is at least 39 when it is huge and at most -45 when it is
/// tiny, whatever the type.
bool is_underflow(const char* b, const char* e) noexcept {
  if (*b == '-') {
    ++b;
  }
  std::int64_t k = 0;
  bool significant = false;
  bool fraction = false;
  for (; b != e && *b != 'e' && *b != 'E'; ++b) {
    if (*b == '.') {
      fraction = true;
      continue;
    }
    significant = significant || *b != '0';
    if (!fraction && significant) {
      ++k;
    } else if (fraction && !significant) {
      --k;
    }
  }
  if (b != e) {
    ++b;
    bool const negative = b != e && *b == '-';
    if (b != e && (*b == '-' || *b == '+')) {
      ++b;
    }
    // Saturating far beyond any feasible input length keeps the sign of k.
    constexpr std::int64_t kMaxExponent = std::int64_t(1) << 56;
    std::int64_t exponent = 0;
    for (; b != e && *b >= '0' && *b <= '9'; ++b) {
      exponent = std::min(exponent * 10 + (*b - '0'), kMaxExponent);
    }
    k += negative ? -exponent : exponent;
  }
  return k < 0;
}

/// `std::from_chars` leaves the output unmodified on `result_out_of_range`,
/// and libstdc++ reports that for subnormal results too. A wider type, where
/// those are normal, recovers them. libc++ has no `long double` overload, but
/// it reports out of range only beyond subnormals, so `double` needs no wider
/// type there. The rest saturates the way strtod does.
template <class Tgt>
Tgt out_of_range_value(const char* b, const char* e) noexcept {
  constexpr auto kInf = std::numeric_limits<Tgt>::infinity();
#if FOLLY_HAVE_STD_LONG_DOUBLE_FROM_CHARS
  using Wide = long double;
#else
  using Wide = double;
#endif

  if constexpr (!std::is_same_v<Tgt, Wide>) {
    constexpr auto kMax = static_cast<Wide>(std::numeric_limits<Tgt>::max());
    Wide wide = 0;
    if (std::from_chars(b, e, wide, std::chars_format::general).ec ==
        std::errc()) {
      if (wide > kMax) {
        return kInf;
      }
      if (wide < -kMax) {
        return -kInf;
      }
      return static_cast<Tgt>(wide);
    }
  }

  auto const magnitude = is_underflow(b, e) ? Tgt(0) : kInf;
  return *b == '-' ? -magnitude : magnitude;
}

#endif // FOLLY_HAVE_STD_FLOAT_FROM_CHARS

} // namespace

#if FOLLY_HAVE_STD_FLOAT_FROM_CHARS

/// The fallback for builds without fast_float. Compiled whether or not this
/// build has fast_float, so that the tests cover it either way.
template <class Tgt>
Expected<Tgt, ConversionCode> str_to_floating_std_from_chars(
    std::string_view* src) noexcept {
  auto* e = src->data() + src->size();
  auto* b = skip_leading_space(src->data(), e);
  if (b == e) {
    return makeUnexpected(ConversionCode::EMPTY_INPUT_STRING);
  }

  // std::from_chars rejects a leading '+', which folly accepts; skipping it
  // must not also admit "+-1".
  if (*b == '+') {
    ++b;
    if (b != e && (*b == '+' || *b == '-')) {
      return makeUnexpected(ConversionCode::STRING_TO_FLOAT_ERROR);
    }
  }

  Tgt result = 0;
  auto [ptr, ec] = std::from_chars(b, e, result, std::chars_format::general);
  if (ec == std::errc::result_out_of_range) {
    result = out_of_range_value<Tgt>(b, ptr);
  } else if (ec != std::errc()) {
    return makeUnexpected(ConversionCode::STRING_TO_FLOAT_ERROR);
  }

  src->remove_prefix(size_t(ptr - src->data()));
  return result;
}

template Expected<float, ConversionCode> str_to_floating_std_from_chars<float>(
    std::string_view* src) noexcept;
template Expected<double, ConversionCode>
str_to_floating_std_from_chars<double>(std::string_view* src) noexcept;

#endif // FOLLY_HAVE_STD_FLOAT_FROM_CHARS

#if !FOLLY_USE_FAST_FLOAT && !FOLLY_HAVE_STD_FLOAT_FROM_CHARS
#error "folly::to<float> needs either fast_float or std::from_chars for floats"
#endif

template <class Tgt>
Expected<Tgt, ConversionCode> str_to_floating_from_chars(
    std::string_view* src) noexcept {
#if FOLLY_USE_FAST_FLOAT
  auto* e = src->data() + src->size();
  auto* b = skip_leading_space(src->data(), e);
  if (b == e) {
    return makeUnexpected(ConversionCode::EMPTY_INPUT_STRING);
  }

  Tgt result;
  fast_float::parse_options options{
      fast_float::chars_format::general |
      fast_float::chars_format::allow_leading_plus};
  // Out of range is not an error: fast_float saturates, which is what folly
  // reports.
  auto [ptr, ec] = fast_float::from_chars_advanced(b, e, result, options);
  if (ec != std::errc() && ec != std::errc::result_out_of_range) {
    return makeUnexpected(ConversionCode::STRING_TO_FLOAT_ERROR);
  }

  src->remove_prefix(size_t(ptr - src->data()));
  return result;
#else
  return str_to_floating_std_from_chars<Tgt>(src);
#endif
}

template Expected<float, ConversionCode> str_to_floating_from_chars<float>(
    std::string_view* src) noexcept;
template Expected<double, ConversionCode> str_to_floating_from_chars<double>(
    std::string_view* src) noexcept;

/**
 * StringPiece to double, with progress information. Alters the
 * StringPiece parameter to munch the already-parsed characters.
 */
template <class Tgt>
Expected<Tgt, ConversionCode> str_to_floating(StringPiece* src) noexcept {
  return detail::str_to_floating_from_chars<Tgt>(src);
}

template Expected<float, ConversionCode> str_to_floating<float>(
    StringPiece* src) noexcept;
template Expected<double, ConversionCode> str_to_floating<double>(
    StringPiece* src) noexcept;

namespace {

/**
 * This class takes care of additional processing needed for signed values,
 * like leading sign character and overflow checks.
 */
template <typename T, bool IsSigned = is_signed_v<T>>
class SignedValueHandler;

template <typename T>
class SignedValueHandler<T, true> {
 public:
  ConversionCode init(const char*& b) {
    negative_ = false;
    if (!std::isdigit(*b)) {
      if (*b == '-') {
        negative_ = true;
      } else if (*b != '+') [[unlikely]] {
        return ConversionCode::INVALID_LEADING_CHAR;
      }
      ++b;
    }
    return ConversionCode::SUCCESS;
  }

  ConversionCode overflow() {
    return negative_
        ? ConversionCode::NEGATIVE_OVERFLOW
        : ConversionCode::POSITIVE_OVERFLOW;
  }

  template <typename U>
  Expected<T, ConversionCode> finalize(U value) {
    T rv;
    if (negative_) {
      FOLLY_PUSH_WARNING
      FOLLY_MSVC_DISABLE_WARNING(4146)

      // unary minus operator applied to unsigned type, result still unsigned
      rv = T(-value);

      FOLLY_POP_WARNING

      if (rv > 0) [[unlikely]] {
        return makeUnexpected(ConversionCode::NEGATIVE_OVERFLOW);
      }
    } else {
      rv = T(value);
      if (rv < 0) [[unlikely]] {
        return makeUnexpected(ConversionCode::POSITIVE_OVERFLOW);
      }
    }
    return rv;
  }

 private:
  bool negative_;
};

// For unsigned types, we don't need any extra processing
template <typename T>
class SignedValueHandler<T, false> {
 public:
  ConversionCode init(const char*&) { return ConversionCode::SUCCESS; }

  ConversionCode overflow() { return ConversionCode::POSITIVE_OVERFLOW; }

  Expected<T, ConversionCode> finalize(T value) { return value; }
};

} // namespace

/**
 * String represented as a pair of pointers to char to signed/unsigned
 * integrals. Assumes NO whitespace before or after, and also that the
 * string is composed entirely of digits (and an optional sign only for
 * signed types). String may be empty, in which case digits_to returns
 * an appropriate error.
 */
template <class Tgt>
inline Expected<Tgt, ConversionCode> digits_to(
    const char* b, const char* const e) noexcept {
  using UT = make_unsigned_t<Tgt>;
  assert(b <= e);

  SignedValueHandler<Tgt> sgn;

  auto err = sgn.init(b);
  if (err != ConversionCode::SUCCESS) [[unlikely]] {
    return makeUnexpected(err);
  }

  auto size = size_t(e - b);

  /* Although the string is entirely made of digits, we still need to
   * check for overflow.
   */
  if (size > std::numeric_limits<UT>::digits10) {
    // Leading zeros?
    if (b < e && *b == '0') {
      for (++b;; ++b) {
        if (b == e) {
          return Tgt(0); // just zeros, e.g. "0000"
        }
        if (*b != '0') {
          size = size_t(e - b);
          break;
        }
      }
    }
    if (size > std::numeric_limits<UT>::digits10 &&
        (size != std::numeric_limits<UT>::digits10 + 1 ||
         strncmp(b, MaxString<UT>::value, size) > 0)) {
      return makeUnexpected(sgn.overflow());
    }
  }

  // Here we know that the number won't overflow when
  // converted. Proceed without checks.

  UT result = 0;

  for (; e - b >= 4; b += 4) {
    result *= UT(10000);
    const int32_t r0 = shift1000[static_cast<size_t>(b[0])];
    const int32_t r1 = shift100[static_cast<size_t>(b[1])];
    const int32_t r2 = shift10[static_cast<size_t>(b[2])];
    const int32_t r3 = shift1[static_cast<size_t>(b[3])];
    const auto sum = r0 + r1 + r2 + r3;
    if (sum >= OOR) {
      goto outOfRange;
    }
    result += UT(sum);
  }

  switch (e - b) {
    case 3: {
      const int32_t r0 = shift100[static_cast<size_t>(b[0])];
      const int32_t r1 = shift10[static_cast<size_t>(b[1])];
      const int32_t r2 = shift1[static_cast<size_t>(b[2])];
      const auto sum = r0 + r1 + r2;
      if (sum >= OOR) {
        goto outOfRange;
      }
      result = UT(1000 * result + sum);
      break;
    }
    case 2: {
      const int32_t r0 = shift10[static_cast<size_t>(b[0])];
      const int32_t r1 = shift1[static_cast<size_t>(b[1])];
      const auto sum = r0 + r1;
      if (sum >= OOR) {
        goto outOfRange;
      }
      result = UT(100 * result + sum);
      break;
    }
    case 1: {
      const int32_t sum = shift1[static_cast<size_t>(b[0])];
      if (sum >= OOR) {
        goto outOfRange;
      }
      result = UT(10 * result + sum);
      break;
    }
    default:
      assert(b == e);
      if (size == 0) {
        return makeUnexpected(ConversionCode::NO_DIGITS);
      }
      break;
  }

  return sgn.finalize(result);

outOfRange:
  return makeUnexpected(ConversionCode::NON_DIGIT_CHAR);
}

template Expected<char, ConversionCode> digits_to<char>(
    const char*, const char*) noexcept;
template Expected<signed char, ConversionCode> digits_to<signed char>(
    const char*, const char*) noexcept;
template Expected<unsigned char, ConversionCode> digits_to<unsigned char>(
    const char*, const char*) noexcept;

template Expected<short, ConversionCode> digits_to<short>(
    const char*, const char*) noexcept;
template Expected<unsigned short, ConversionCode> digits_to<unsigned short>(
    const char*, const char*) noexcept;

template Expected<int, ConversionCode> digits_to<int>(
    const char*, const char*) noexcept;
template Expected<unsigned int, ConversionCode> digits_to<unsigned int>(
    const char*, const char*) noexcept;

template Expected<long, ConversionCode> digits_to<long>(
    const char*, const char*) noexcept;
template Expected<unsigned long, ConversionCode> digits_to<unsigned long>(
    const char*, const char*) noexcept;

template Expected<long long, ConversionCode> digits_to<long long>(
    const char*, const char*) noexcept;
template Expected<unsigned long long, ConversionCode>
digits_to<unsigned long long>(const char*, const char*) noexcept;

#if FOLLY_HAVE_INT128_T
template Expected<__int128, ConversionCode> digits_to<__int128>(
    const char*, const char*) noexcept;
template Expected<unsigned __int128, ConversionCode>
digits_to<unsigned __int128>(const char*, const char*) noexcept;
#endif

/**
 * String view to integrals, with progress information. Alters the
 * view parameter to munch the already-parsed characters.
 */
template <class Tgt>
Expected<Tgt, ConversionCode> str_to_integral(std::string_view* src) noexcept {
  using UT = make_unsigned_t<Tgt>;

  auto b = src->data(), past = src->data() + src->size();

  for (;; ++b) {
    if (b >= past) [[unlikely]] {
      return makeUnexpected(ConversionCode::EMPTY_INPUT_STRING);
    }
    if ((*b < '\t' || *b > '\r') && *b != ' ') {
      break;
    }
  }

  SignedValueHandler<Tgt> sgn;
  auto err = sgn.init(b);

  if (err != ConversionCode::SUCCESS) [[unlikely]] {
    return makeUnexpected(err);
  }
  if (is_signed_v<Tgt> && FOLLY_UNLIKELY(b >= past)) {
    return makeUnexpected(ConversionCode::NO_DIGITS);
  }
  if (!isdigit(*b)) [[unlikely]] {
    return makeUnexpected(ConversionCode::NON_DIGIT_CHAR);
  }

  auto m = findFirstNonDigit(b + 1, past);

  auto tmp = digits_to<UT>(b, m);

  if (!tmp.hasValue()) [[unlikely]] {
    return makeUnexpected(
        tmp.error() == ConversionCode::POSITIVE_OVERFLOW
            ? sgn.overflow()
            : tmp.error());
  }

  auto res = sgn.finalize(tmp.value());

  if (res.hasValue()) {
    src->remove_prefix(size_t(m - src->data()));
  }

  return res;
}

template Expected<char, ConversionCode> str_to_integral<char>(
    std::string_view* src) noexcept;
template Expected<signed char, ConversionCode> str_to_integral<signed char>(
    std::string_view* src) noexcept;
template Expected<unsigned char, ConversionCode> str_to_integral<unsigned char>(
    std::string_view* src) noexcept;

template Expected<short, ConversionCode> str_to_integral<short>(
    std::string_view* src) noexcept;
template Expected<unsigned short, ConversionCode>
str_to_integral<unsigned short>(std::string_view* src) noexcept;

template Expected<int, ConversionCode> str_to_integral<int>(
    std::string_view* src) noexcept;
template Expected<unsigned int, ConversionCode> str_to_integral<unsigned int>(
    std::string_view* src) noexcept;

template Expected<long, ConversionCode> str_to_integral<long>(
    std::string_view* src) noexcept;
template Expected<unsigned long, ConversionCode> str_to_integral<unsigned long>(
    std::string_view* src) noexcept;

template Expected<long long, ConversionCode> str_to_integral<long long>(
    std::string_view* src) noexcept;
template Expected<unsigned long long, ConversionCode>
str_to_integral<unsigned long long>(std::string_view* src) noexcept;

#if FOLLY_HAVE_INT128_T
template Expected<__int128, ConversionCode> str_to_integral<__int128>(
    std::string_view* src) noexcept;
template Expected<unsigned __int128, ConversionCode>
str_to_integral<unsigned __int128>(std::string_view* src) noexcept;
#endif
size_t formatDouble(double value, char* buf, size_t bufSize) {
  auto result = fmt::format_to_n(buf, bufSize, "{}", value);
  return result.out - buf;
}

} // namespace detail

ConversionError makeConversionError(ConversionCode code, StringPiece input) {
  using namespace detail;
  static_assert(
      std::is_unsigned<std::underlying_type<ConversionCode>::type>::value,
      "ConversionCode should be unsigned");
  auto index = static_cast<std::size_t>(code);
  FOLLY_SAFE_CHECK(index < kErrorStrings.size(), "code=", uint64_t(index));
  const ErrorString& err = kErrorStrings[index];
  if (code == ConversionCode::EMPTY_INPUT_STRING && input.empty()) {
    return {err.string, code};
  }
  std::string tmp(err.string);
  tmp.append(": ");
  if (err.quote) {
    tmp.append(1, '"');
  }
  if (!input.empty()) {
    tmp.append(input.data(), input.size());
  }
  if (err.quote) {
    tmp.append(1, '"');
  }
  return {tmp, code};
}

} // namespace folly
