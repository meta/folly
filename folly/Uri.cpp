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

#include <folly/Uri.h>

#include <algorithm>
#include <cctype>

#include <boost/regex.hpp>

namespace folly {

namespace {

std::string submatch(const boost::cmatch& m, int idx) {
  const auto& sub = m[idx];
  return std::string(sub.first, sub.second);
}

} // namespace

// private default constructor
Uri::Uri() : hasAuthority_(false), port_(0) {}

// public string constructor
Uri::Uri(std::string_view str) : hasAuthority_(false), port_(0) {
  auto maybeUri = tryFromString(str);
  if (maybeUri.hasError()) {
    switch (maybeUri.error()) {
      case UriFormatError::INVALID_URI_AUTHORITY:
        throw std::invalid_argument(
            to<std::string>("invalid URI authority: ", str));
      case UriFormatError::INVALID_URI_PORT:
        throw std::invalid_argument(to<std::string>("invalid URI port: ", str));
      case UriFormatError::INVALID_URI:
      default:
        throw std::invalid_argument(to<std::string>("invalid URI: ", str));
    }
  }
  *this = maybeUri.value();
}

Expected<Uri, UriFormatError> Uri::tryFromString(
    std::string_view str) noexcept {
  Uri result;

  static const auto& uriRegex = *new boost::regex(
      "([a-zA-Z][a-zA-Z0-9+.-]*):" // scheme:
      "([^?#]*)" // authority and path
      "(?:\\?([^#]*))?" // ?query
      "(?:#(.*))?"); // #fragment
  static const auto& authorityAndPathRegex =
      *new boost::regex("//([^/]*)(/.*)?");

  boost::cmatch match;
  if (FOLLY_UNLIKELY(!boost::regex_match(
          str.data(), str.data() + str.size(), match, uriRegex))) {
    return makeUnexpected(UriFormatError::INVALID_URI);
  }

  result.scheme_ = submatch(match, 1);
  std::transform(
      result.scheme_.begin(),
      result.scheme_.end(),
      result.scheme_.begin(),
      ::tolower);

  std::string_view authorityAndPath(match[2].first, match[2].second);
  boost::cmatch authorityAndPathMatch;
  if (!boost::regex_match(
          authorityAndPath.data(),
          authorityAndPath.data() + authorityAndPath.size(),
          authorityAndPathMatch,
          authorityAndPathRegex)) {
    // Does not start with //, doesn't have authority
    result.hasAuthority_ = false;
    result.path_ = std::string(authorityAndPath);
  } else {
    static const auto& authorityRegex = *new boost::regex(
        "(?:([^@:]*)(?::([^@]*))?@)?" // username, password
        "(\\[[^\\]]*\\]|[^\\[:]*)" // host (IP-literal (e.g. '['+IPv6+']',
                                   // dotted-IPv4, or named host)
        "(?::(\\d*))?"); // port

    const auto authority = authorityAndPathMatch[1];
    boost::cmatch authorityMatch;
    if (!boost::regex_match(
            authority.first,
            authority.second,
            authorityMatch,
            authorityRegex)) {
      return makeUnexpected(UriFormatError::INVALID_URI_AUTHORITY);
    }

    std::string_view port(authorityMatch[4].first, authorityMatch[4].second);
    if (!port.empty()) {
      try {
        result.port_ = to<uint16_t>(port);
      } catch (ConversionError const&) {
        return makeUnexpected(UriFormatError::INVALID_URI_PORT);
      }
    }

    result.hasAuthority_ = true;
    result.username_ = submatch(authorityMatch, 1);
    result.password_ = submatch(authorityMatch, 2);
    result.host_ = submatch(authorityMatch, 3);
    result.path_ = submatch(authorityAndPathMatch, 2);
  }

  result.query_ = submatch(match, 3);
  result.fragment_ = submatch(match, 4);

  return result;
}

std::string Uri::authority() const {
  std::string result;

  // Port is 5 characters max and we have up to 3 delimiters.
  result.reserve(host().size() + username().size() + password().size() + 8);

  if (!username().empty() || !password().empty()) {
    result.append(username());

    if (!password().empty()) {
      result.push_back(':');
      result.append(password());
    }

    result.push_back('@');
  }

  result.append(host());

  if (port() != 0) {
    result.push_back(':');
    toAppend(port(), &result);
  }

  return result;
}

std::string Uri::hostname() const {
  if (!host_.empty() && host_[0] == '[') {
    // If it starts with '[', then it should end with ']', this is ensured by
    // regex
    return host_.substr(1, host_.size() - 2);
  }
  return host_;
}

const std::vector<std::pair<std::string, std::string>>& Uri::getQueryParams() {
  if (!query_.empty() && queryParams_.empty()) {
    // Parse query string
    static const auto& queryParamRegex = *new boost::regex(
        "(^|&)" /*start of query or start of parameter "&"*/
        "([^=&]*)=?" /*parameter name and "=" if value is expected*/
        "([^=&]*)" /*parameter value*/
        "(?=(&|$))" /*forward reference, next should be end of query or
                      start of next parameter*/);
    const boost::cregex_iterator paramBeginItr(
        query_.data(), query_.data() + query_.size(), queryParamRegex);
    boost::cregex_iterator paramEndItr;
    for (auto itr = paramBeginItr; itr != paramEndItr; ++itr) {
      if (itr->length(2) == 0) {
        // key is empty, ignore it
        continue;
      }
      queryParams_.emplace_back(
          std::string((*itr)[2].first, (*itr)[2].second), // parameter name
          std::string((*itr)[3].first, (*itr)[3].second) // parameter value
      );
    }
  }
  return queryParams_;
}

} // namespace folly
