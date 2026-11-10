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

#include <folly/Range.h>

// This needs to be at the end because some versions end up including
// Windows.h without defining NOMINMAX, which breaks uses of std::numeric_limits
// in various headers that we include. EXPECT_PCRE_MATCH keeps Boost.Regex for
// its Perl syntax, which existing patterns rely on.
// @lint-ignore CLANGTIDY facebook-hte-BadInclude-boost/regex.hpp
#include <boost/regex.hpp>

// Declared in folly/testing/TestUtil.h, which this target cannot include: the
// test_util target that owns the header depends on this one.
namespace folly::test::detail {

bool hasPCREPatternMatch(StringPiece pattern, StringPiece target) {
  return boost::regex_match(
      target.begin(),
      target.end(),
      // @lint-ignore CLANGTIDY facebook-hte-BoostRegexRisky
      boost::regex(pattern.begin(), pattern.end()));
}

bool hasNoPCREPatternMatch(StringPiece pattern, StringPiece target) {
  return !hasPCREPatternMatch(pattern, target);
}

} // namespace folly::test::detail
