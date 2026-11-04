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

#include <cstdint>
#include <initializer_list>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <folly/detail/base64_detail/Base64Api.h>
#include <folly/detail/base64_detail/Base64Scalar.h>
#include <folly/detail/base64_detail/Base64Simd.h>
#include <folly/detail/base64_detail/Base64_SSE4_2.h>
#include <folly/portability/GTest.h>

namespace folly::detail::base64_detail {
namespace {

struct TestCase {
  std::string_view data;
  std::string_view encodedStd;
  std::string_view encodedURL;
};

// The types are weird because we need to do constexpr tests
struct TestCaseOnStack : TestCase {
  constexpr TestCaseOnStack(
      std::initializer_list<char> dataInit, // initializer list makes it easier
                                            // to put non printable characters
                                            // compared to strings
      std::string_view encodedStdParam,
      std::string_view encodedURLParam)
      : TestCase{{}, encodedStdParam, encodedURLParam}, dataBuf{} {
    // std::copy not constexpr
    auto f = dataInit.begin(), l = dataInit.end();
    auto o = dataBuf.begin();

    while (f != l) {
      *o++ = *f++;
    }
    data = std::string_view{
        dataBuf.data(), static_cast<size_t>(o - dataBuf.begin())};
  }

  std::array<char, 200> dataBuf;
} constexpr staticTestCases[] = {
    // clang-format off
    {
      std::initializer_list<char>{}, "", ""
    },
    {
        { 0, 0, 0 }, "AAAA", "AAAA"
    },
    {
        {1}, "AQ==", "AQ"
    },
    {
        {1, 0}, "AQA=", "AQA",
    },
    {
        {1, 0, 0}, "AQAA", "AQAA"
    },
    {
        {'a','b','c','d'},
        "YWJjZA==",
        "YWJjZA"
    },
    {
        {'a', 'b', 'c'},
        "YWJj",
        "YWJj"
    },
    {
      {'l','e','s','s',' ',
       'i','s',' ',
       'm','o','r','e',' ',
       't','h','a','n',' ',
       'm','o','r','e'},
      "bGVzcyBpcyBtb3JlIHRoYW4gbW9yZQ==",
      "bGVzcyBpcyBtb3JlIHRoYW4gbW9yZQ",
    },
    {
      {'<','>', '?','s','u'},
      "PD4/c3U=",
      "PD4_c3U"
    },
    // clang-format on
};

std::string byteRangeToString(std::string_view data) {
  std::stringstream res;
  res << '{';
  auto f = data.begin();
  auto l = data.end();

  if (f != l) {
    res << static_cast<std::int32_t>(*f);
    while (++f != l) {
      res << ", " << static_cast<std::int32_t>(*f);
    }
  }
  res << '}';
  return std::move(res).str();
}

template <typename TestRunner>
constexpr bool staticTests(TestRunner testRunner) {
  for (const auto& test : staticTestCases) {
    if (!testRunner(TestCase{test})) {
      return false;
    }
  }
  return true;
}

template <typename I, typename N, typename V>
constexpr I fill_n(I f, N n, V v) {
  while (n--) {
    *f++ = v;
  }
  return f;
}

template <typename TestRunner>
constexpr bool manyZeroesTests(TestRunner testRunner) {
  for (std::size_t inSize = 0; inSize != 129; ++inSize) {
    TestCase test;

    // Populate input
    std::array<char, 256> buf = {}; // fill in 0s
    buf[inSize + 1] = 15; // messing with the input
    test.data = {buf.data(), inSize};

    // Populate expected
    std::array<char, 256> expectedBuf = {};
    char* expectedL = fill_n(expectedBuf.begin(), inSize / 3 * 4, 'A');
    char* expectedURLL = expectedL;

    if (inSize % 3 == 2) {
      *expectedL++ = 'A';
      *expectedL++ = 'A';
      *expectedL++ = 'A';
      expectedURLL = expectedL;
      *expectedL++ = '=';
    } else if (inSize % 3 == 1) {
      *expectedL++ = 'A';
      *expectedL++ = 'A';
      expectedURLL = expectedL;
      *expectedL++ = '=';
      *expectedL++ = '=';
    }

    test.encodedStd =
        std::string_view(expectedBuf.data(), expectedL - expectedBuf.data());
    test.encodedURL =
        std::string_view(expectedBuf.data(), expectedURLL - expectedBuf.data());

    // Run test
    if (!testRunner(test)) {
      return false;
    }
  }

  return true;
}

template <typename TestRunner>
constexpr bool runEncodeTests(TestRunner testRunner) {
  return staticTests(testRunner) && manyZeroesTests(testRunner);
}

// In constexpr we can have a non constexpr expression, as long
// as it is not evaluated.
//
// There was a gcc bug with respect to it. Luckily this just affects how
// much useful information will be output in case of a test failure.
#if defined(__GNUC__) && !defined(__clang__)
#define GCC_CONSTEXPR_BUG_ACTIVE
#endif

enum DecoderType { URLDecoder, RegularDecoder, PHPStrictDecoder };

template <DecoderType decoderType>
struct ConstexprTester {
  constexpr bool encodeTest(TestCase test) const {
    std::array<char, 1000> buf = {};
    char* end =
        base64EncodeScalar(test.data.begin(), test.data.end(), buf.data());
    std::string_view actual(buf.data(), end - buf.data());

    if (test.encodedStd == actual) {
      return true;
    }

#ifndef GCC_CONSTEXPR_BUG_ACTIVE
    EXPECT_EQ(test.encodedStd, actual)
        << "Regular encoding mismatch. Input data:\n"
        << byteRangeToString(test.data);
#endif

    return false;
  }

  constexpr bool decodeTest(TestCase test) const {
    std::array<char, 1000> buf = {};
    auto res = base64DecodeScalar(
        test.encodedStd.data(),
        test.encodedStd.data() + test.encodedStd.size(),
        buf.begin());

    std::string_view decoded(buf.begin(), res.o - buf.begin());

    if (res.isSuccess && test.data == decoded) {
      return true;
    }

#ifndef GCC_CONSTEXPR_BUG_ACTIVE
    EXPECT_TRUE(res.isSuccess) << "encoded: " << test.encodedStd;
    EXPECT_EQ(test.data, decoded) << "encoded: " << test.encodedStd;
#endif

    return false;
  }

  constexpr bool encodeURLTest(TestCase test) const {
    std::array<char, 1000> buf = {};
    char* end =
        base64URLEncodeScalar(test.data.begin(), test.data.end(), buf.data());
    std::string_view actual(buf.data(), end - buf.data());

    if (test.encodedURL == actual) {
      return true;
    }

#ifndef GCC_CONSTEXPR_BUG_ACTIVE
    EXPECT_EQ(test.encodedURL, actual)
        << "URL encoding mismatch. Input data:\n"
        << byteRangeToString(test.data);
#endif

    return false;
  }

  constexpr bool decodeURLTest(TestCase test) const {
    auto oneInput = [&](std::string_view encoded) {
      std::array<char, 1000> buf = {};
      auto res = base64URLDecodeScalar(
          encoded.data(), encoded.data() + encoded.size(), buf.begin());

      std::string_view decoded(buf.begin(), res.o - buf.begin());
      if (res.isSuccess && test.data == decoded) {
        return true;
      }

#ifndef GCC_CONSTEXPR_BUG_ACTIVE
      EXPECT_TRUE(res.isSuccess) << "encoded: " << encoded;
      EXPECT_EQ(test.data, decoded) << "encoded: " << encoded;
#endif

      return false;
    };

    return oneInput(test.encodedStd) && oneInput(test.encodedURL);
  }

  constexpr bool decodePHPTest(TestCase test) const {
    std::array<char, 1000> buf = {};
    auto res = base64PHPStrictDecode(
        test.encodedStd.data(),
        test.encodedStd.data() + test.encodedStd.size(),
        buf.begin());

    std::string_view decoded(buf.begin(), res.o - buf.begin());

    if (res.isSuccess && test.data == decoded) {
      return true;
    }

#ifndef GCC_CONSTEXPR_BUG_ACTIVE
    EXPECT_TRUE(res.isSuccess) << "encoded: " << test.encodedStd;
    EXPECT_EQ(test.data, decoded) << "encoded: " << test.encodedStd;
#endif

    return false;
  }

  constexpr bool sizeTests(TestCase test) const {
    std::size_t encodedSize = base64EncodedSize(test.data.size());
    std::size_t encodedURLSize = base64URLEncodedSize(test.data.size());
    std::size_t decodedSize = base64DecodedSize(
        test.encodedStd.data(),
        test.encodedStd.data() + test.encodedStd.size());
    std::size_t decodedURLSize = base64URLDecodedSize(
        test.encodedURL.data(),
        test.encodedURL.data() + test.encodedURL.size());
    std::size_t decodedStdWithURlSize = base64URLDecodedSize(
        test.encodedStd.data(),
        test.encodedStd.data() + test.encodedStd.size());
    std::size_t decodedPHPSize = base64PHPStrictDecodeRequiredOutputSize(
        test.encodedStd.data(),
        test.encodedStd.data() + test.encodedStd.size());

    if (encodedSize == test.encodedStd.size() &&
        encodedURLSize == test.encodedURL.size() &&
        decodedSize == test.data.size() && decodedURLSize == test.data.size() &&
        decodedStdWithURlSize == test.data.size() &&
        decodedPHPSize >= test.data.size()) {
      return true;
    }

#ifndef GCC_CONSTEXPR_BUG_ACTIVE
    EXPECT_EQ(test.encodedStd.size(), encodedSize) << test.encodedStd;
    EXPECT_EQ(test.encodedURL.size(), encodedURLSize) << test.encodedURL;
    EXPECT_EQ(test.data.size(), decodedSize) << test.encodedStd;
    EXPECT_EQ(test.data.size(), decodedStdWithURlSize) << test.encodedStd;
    EXPECT_EQ(test.data.size(), decodedURLSize) << test.encodedURL;
    EXPECT_LE(test.data.size(), decodedPHPSize) << test.encodedStd;
#endif

    return false;
  }

  constexpr bool operator()(TestCase test) const {
    if (!sizeTests(test)) {
      return false;
    }

    if constexpr (decoderType == DecoderType::RegularDecoder) {
      return encodeTest(test) && decodeTest(test);
    }

    if constexpr (decoderType == DecoderType::URLDecoder) {
      return encodeURLTest(test) && decodeURLTest(test);
    }

    if constexpr (decoderType == DecoderType::PHPStrictDecoder) {
      return decodePHPTest(test);
    }

    return true;
  }
};

struct SimdTester {
  using Encode = char* (*)(const char*, const char*, char*);
  using Decode = Base64DecodeResult (*)(const char*, const char*, char*);

  Encode encode;
  Encode encodeURL;
  Decode decode;
  Decode decodeURL;

  bool encodeTest(TestCase test) const {
    std::string actual(base64EncodedSize(test.data.size()), 0);
    encode(test.data.begin(), test.data.end(), actual.data());
    if (test.encodedStd == actual) {
      return true;
    }
    EXPECT_EQ(test.encodedStd, actual)
        << "Regular encoding mismatch. Input data:\n"
        << byteRangeToString(test.data);
    return false;
  }

  bool encodeURLTest(TestCase test) const {
    std::string actual(base64URLEncodedSize(test.data.size()), 0);
    encodeURL(test.data.begin(), test.data.end(), actual.data());
    if (test.encodedURL == actual) {
      return true;
    }
    EXPECT_EQ(test.encodedStd, actual)
        << "URL encoding mismatch. Input data:\n"
        << byteRangeToString(test.data);
    return false;
  }

  bool decodeTest(TestCase test) const {
    std::string actual(
        base64DecodedSize(
            test.encodedStd.data(),
            test.encodedStd.data() + test.encodedStd.size()),
        0);
    auto decodedResult = decode(
        test.encodedStd.data(),
        test.encodedStd.data() + test.encodedStd.size(),
        actual.data());

    if (decodedResult.isSuccess && test.data == actual) {
      return true;
    }

    EXPECT_TRUE(decodedResult.isSuccess) << byteRangeToString(test.data);
    EXPECT_EQ(test.data, actual) << byteRangeToString(test.data);
    return false;
  }

  bool decodeURLTest(TestCase test) const {
    auto oneInput = [&](std::string_view encoded) {
      std::string decoded(
          base64URLDecodedSize(encoded.data(), encoded.data() + encoded.size()),
          0);
      auto res = decodeURL(
          encoded.data(), encoded.data() + encoded.size(), decoded.data());

      if (res.isSuccess && test.data == decoded) {
        return true;
      }

      EXPECT_TRUE(res.isSuccess) << "encoded: " << encoded;
      EXPECT_EQ(test.data, decoded) << "encoded: " << encoded;
      return false;
    };

    return oneInput(test.encodedStd) && oneInput(test.encodedURL);
  }

  bool operator()(TestCase test) const {
    return encodeTest(test) && encodeURLTest(test) && decodeTest(test) &&
        decodeURLTest(test);
  }
};

TEST(Base64, ConstexprTests) {
  // Comment out the static assert to debug
  static_assert(runEncodeTests(ConstexprTester<DecoderType::RegularDecoder>{}));
  static_assert(runEncodeTests(ConstexprTester<DecoderType::URLDecoder>{}));
  static_assert(
      runEncodeTests(ConstexprTester<DecoderType::PHPStrictDecoder>{}));
  ASSERT_TRUE(runEncodeTests(ConstexprTester<DecoderType::RegularDecoder>{}));
  ASSERT_TRUE(runEncodeTests(ConstexprTester<DecoderType::URLDecoder>{}));
  ASSERT_TRUE(runEncodeTests(ConstexprTester<DecoderType::PHPStrictDecoder>{}));
}

TEST(Base64, SpecialCases) {
  ASSERT_TRUE(runEncodeTests(
      SimdTester{
          base64EncodeScalar,
          base64URLEncodeScalar,
          base64DecodeSWAR,
          base64URLDecodeSWAR}));
#if FOLLY_SSE_PREREQ(4, 2)
  ASSERT_TRUE(runEncodeTests(
      SimdTester{
          base64Encode_SSE4_2,
          base64URLEncode_SSE4_2,
          base64Decode_SSE4_2,
          base64URLDecodeSWAR}));
#endif
}

constexpr char kHasNegative0[] = {'A', 'b', 'c', -15, '\0'};
constexpr char kHasNegative1[] = {
    'a', 'b', 'c', 'd', 'a', -15, 'c', 'd', 'a', 'b', 'c',
    'd', 'a', 'b', 'c', 'd', 'a', 'b', 'c', 'd', '\0'};

struct DecodingErrorDetectionTest {
  bool isSuccess;
  std::string_view input;
} constexpr kDecodingErrorDetection[] = {
    // clang-format off
    { true,  "" },
    { false, "=" },
    { false, "==" },
    { false, "A" },
    { false, "B=" },
    { false, "ba=" },
    { true,  "0w==" },
    { true,  "000=" },
    { false, "===" },
    { false, "0===" },
    { false, "aa=0" },
    { false, "aaaa""aaaa""aaaa""aaaa""0" },
    { true,  "aaaa""aaaa""aaaa""aaaa""0w==" },
    { true,  "0aaa""aaaa""aaaa""aaaa""aaaa""aaaa" },
    { false, "$aaa""aaaa""aaaa""aaaa""aaaa""aaaa" },
    { false, "aaaa""aa$a""aaaa""aaaa""aaaa""aaaa" },
    { false, "aaaaa"},
    { false, kHasNegative0 },
    { false, kHasNegative1 },
    // clang-format on
};

constexpr std::string_view kDecodingOnlyURLValid[] = {
    "ba",
    "ba__",
    "ba__ba--ba__",
    "bA_/0a--ba+_",
    "_-==",
    "iZ==",
    "00==",
    "997=",
    "+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-",
    "bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_"
    "bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_",
    "bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba+_bA_/0a--ba==",
};

constexpr std::string_view kDecodingOnlyStrictPHPValid[] = {
    " ",
    "\n",
    "\t",
    "\r",
    "\t\n\r ",
    "\n        \n",
    "\n\n\n\n\n\n\n\n",
    "\ta\t\na\n\ra\r a ",
    "a\n\na\n\na\n\na\n\n"
    "0w\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n=\n\n=\n\n",
    "\r\r\r\r\r\r\r\r\r  0  0  0  \t\t\t\t\n\n\n\n   =   \n\n\n\n",
};

template <DecoderType decoderType>
constexpr size_t decodedSize(std::string_view in) {
  const char* f = in.data();
  const char* l = in.data() + in.size();

  if constexpr (decoderType == DecoderType::URLDecoder) {
    return base64URLDecodedSize(f, l);
  } else if constexpr (decoderType == DecoderType::PHPStrictDecoder) {
    return base64PHPStrictDecodeRequiredOutputSize(f, l);
  } else {
    return base64DecodedSize(f, l);
  }
}

template <DecoderType decoderType, typename Decoder>
void triggerASANOnBadDecode(std::string_view in, Decoder decoder) {
  std::vector<char> buf(decodedSize<decoderType>(in));
  decoder(in.data(), in.data() + in.size(), buf.data());
}

template <DecoderType decoderType, typename Decoder>
constexpr bool decodingErrorDetectionTest(Decoder decoder) {
  std::array<char, 1000> buf = {};

  auto sizeTest = [&](std::string_view in, Base64DecodeResult r) {
    std::size_t allocatedSize = decodedSize<decoderType>(in);

    std::size_t usedSize = static_cast<std::size_t>(r.o - buf.data());

    if constexpr (decoderType == DecoderType::PHPStrictDecoder) {
      if (usedSize <= allocatedSize) {
        return true;
      }
    } else {
      if (usedSize == allocatedSize) {
        return true;
      }
    }

    if (r.isSuccess) {
#ifndef GCC_CONSTEXPR_BUG_ACTIVE
      if constexpr (decoderType == DecoderType::PHPStrictDecoder) {
        EXPECT_LE(usedSize, allocatedSize) << in;
      } else {
        EXPECT_EQ(usedSize, allocatedSize)
            << in << " isURL: " << (decoderType == DecoderType::URLDecoder);
      }
#endif
      return false;
    }

    if (allocatedSize > 1000 || // overflow
        usedSize > allocatedSize) {
#ifndef GCC_CONSTEXPR_BUG_ACTIVE
      if constexpr (decoderType == DecoderType::PHPStrictDecoder) {
        EXPECT_LE(usedSize, allocatedSize) << in;
      } else {
        EXPECT_LE(usedSize, allocatedSize)
            << in << " isURL: " << (decoderType == DecoderType::URLDecoder);
      }
#endif
      return false;
    }
    return true;
  };

  for (const auto& test : kDecodingErrorDetection) {
    if (!std::is_constant_evaluated()) {
      triggerASANOnBadDecode<decoderType>(test.input, decoder);
    }
    auto r = decoder(
        test.input.data(), test.input.data() + test.input.size(), buf.data());
    if (test.isSuccess != r.isSuccess) {
#ifndef GCC_CONSTEXPR_BUG_ACTIVE
      EXPECT_EQ(test.isSuccess, r.isSuccess) << test.input;
#endif
      return false;
    }
    if (!sizeTest(test.input, r)) {
      return false;
    }
  }

  for (std::string_view URLOnly : kDecodingOnlyURLValid) {
    if (!std::is_constant_evaluated()) {
      triggerASANOnBadDecode<decoderType>(URLOnly, decoder);
    }
    auto r =
        decoder(URLOnly.data(), URLOnly.data() + URLOnly.size(), buf.data());
    auto isURLDecoder = decoderType == DecoderType::URLDecoder;
    if (isURLDecoder != r.isSuccess) {
#ifndef GCC_CONSTEXPR_BUG_ACTIVE
      EXPECT_EQ(isURLDecoder, r.isSuccess) << URLOnly;
#endif
      return false;
    }

    if (!sizeTest(URLOnly, r)) {
      return false;
    }
  }

  for (std::string_view PHPStrictOnly : kDecodingOnlyStrictPHPValid) {
    if (!std::is_constant_evaluated()) {
      triggerASANOnBadDecode<decoderType>(PHPStrictOnly, decoder);
    }
    auto r = decoder(
        PHPStrictOnly.data(),
        PHPStrictOnly.data() + PHPStrictOnly.size(),
        buf.data());
    auto isPHPStrictDecoder = decoderType == DecoderType::PHPStrictDecoder;
    if (isPHPStrictDecoder != r.isSuccess) {
#ifndef GCC_CONSTEXPR_BUG_ACTIVE
      EXPECT_EQ(isPHPStrictDecoder, r.isSuccess) << PHPStrictOnly;
#endif
      return false;
    }

    if (!sizeTest(PHPStrictOnly, r)) {
      return false;
    }
  }

  return true;
}

TEST(Base64, DecodingErrorDetection) {
  static_assert(decodingErrorDetectionTest<DecoderType::RegularDecoder>(
      base64DecodeScalar));
  static_assert(decodingErrorDetectionTest<DecoderType::URLDecoder>(
      base64URLDecodeScalar));
  ASSERT_TRUE(
      decodingErrorDetectionTest<DecoderType::RegularDecoder>(
          base64DecodeScalar));
  ASSERT_TRUE(
      decodingErrorDetectionTest<DecoderType::URLDecoder>(
          base64URLDecodeScalar));
  ASSERT_TRUE(
      decodingErrorDetectionTest<DecoderType::RegularDecoder>(
          base64DecodeSWAR));
  ASSERT_TRUE(
      decodingErrorDetectionTest<DecoderType::URLDecoder>(base64URLDecodeSWAR));
#if FOLLY_SSE_PREREQ(4, 2)
  ASSERT_TRUE(
      decodingErrorDetectionTest<DecoderType::RegularDecoder>(
          base64Decode_SSE4_2));
#endif
  ASSERT_TRUE(
      decodingErrorDetectionTest<DecoderType::PHPStrictDecoder>(
          base64PHPStrictDecode));
}

} // namespace
} // namespace folly::detail::base64_detail
