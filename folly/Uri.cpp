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
#include <optional>

namespace folly {

namespace {

bool isAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isDigit(char c) {
  return c >= '0' && c <= '9';
}

bool isSchemeChar(char c) {
  return isAlpha(c) || isDigit(c) || c == '+' || c == '.' || c == '-';
}

// Parses `s` as a host, either an IP literal in brackets or a possibly empty
// run of characters other than '[' and ':', optionally followed by ':' and a
// possibly empty port.
bool parseHostAndPort(StringPiece s, StringPiece& host, StringPiece& port) {
  size_t hostSize = 0;
  if (s.startsWith('[')) {
    hostSize = s.find(']');
    if (hostSize == StringPiece::npos) {
      return false;
    }
    ++hostSize;
  } else {
    hostSize = std::min(s.find_first_of("[:"), s.size());
  }
  host = s.subpiece(0, hostSize);
  port = s.subpiece(hostSize);
  return port.empty() ||
      (port.removePrefix(':') &&
       std::all_of(port.begin(), port.end(), isDigit));
}

// The `^` and `$` anchors of the query parameter pattern also match at line
// breaks, except between '\r' and '\n'.
bool isLineBreak(char c) {
  return c == '\n' || c == '\r' || c == '\f';
}

bool isCrLf(StringPiece s, size_t i) {
  return i > 0 && i < s.size() && s[i - 1] == '\r' && s[i] == '\n';
}

bool isLineStart(StringPiece s, size_t i) {
  return i == 0 || (isLineBreak(s[i - 1]) && !isCrLf(s, i));
}

bool isLineEnd(StringPiece s, size_t i) {
  return i == s.size() || (isLineBreak(s[i]) && !isCrLf(s, i));
}

struct QueryParam {
  size_t nameEnd;
  size_t valueBegin;
  size_t end;
};

// Matches `([^=&]*)=?([^=&]*)(?=&|$)` at `begin` with backtracking regex
// semantics, trying the longest name and value first.
std::optional<QueryParam> matchQueryParam(StringPiece query, size_t begin) {
  auto findDelimiter = [&](size_t from) {
    return std::min(query.find_first_of("=&", from), query.size());
  };
  size_t nameEnd = findDelimiter(begin);
  size_t valueEnd = nameEnd < query.size() && query[nameEnd] == '='
      ? findDelimiter(nameEnd + 1)
      : nameEnd;
  for (size_t end = valueEnd + 1; end-- > begin;) {
    if (isLineEnd(query, end) || query[end] == '&') {
      return end > nameEnd
          ? QueryParam{nameEnd, nameEnd + 1, end}
          : QueryParam{end, end, end};
    }
  }
  return std::nullopt;
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

  auto colon = str.find(':');
  if (colon == std::string_view::npos || !isAlpha(str[0]) ||
      !std::all_of(str.begin() + 1, str.begin() + colon, isSchemeChar))
      [[unlikely]] {
    return makeUnexpected(UriFormatError::INVALID_URI);
  }

  result.scheme_ = std::string(str.substr(0, colon));
  std::transform(
      result.scheme_.begin(),
      result.scheme_.end(),
      result.scheme_.begin(),
      ::tolower);

  StringPiece rest = str.substr(colon + 1);
  auto hash = rest.find('#');
  if (hash != StringPiece::npos) {
    result.fragment_ = rest.subpiece(hash + 1).str();
    rest = rest.subpiece(0, hash);
  }
  auto question = rest.find('?');
  if (question != StringPiece::npos) {
    result.query_ = rest.subpiece(question + 1).str();
    rest = rest.subpiece(0, question);
  }

  if (!rest.removePrefix("//")) {
    // Does not start with //, doesn't have authority
    result.path_ = rest.str();
    return result;
  }

  StringPiece authority = rest.subpiece(0, rest.find('/'));
  result.path_ = rest.subpiece(authority.size()).str();

  // The userinfo ends at the first '@', but only if the rest is a valid host
  // and port, otherwise the whole authority is the host and port.
  StringPiece host;
  StringPiece port;
  auto at = authority.find('@');
  if (at != StringPiece::npos &&
      parseHostAndPort(authority.subpiece(at + 1), host, port)) {
    StringPiece userinfo = authority.subpiece(0, at);
    result.username_ = userinfo.split_step(':').str();
    result.password_ = userinfo.str();
  } else if (!parseHostAndPort(authority, host, port)) {
    return makeUnexpected(UriFormatError::INVALID_URI_AUTHORITY);
  }

  if (!port.empty()) {
    try {
      result.port_ = to<uint16_t>(port);
    } catch (ConversionError const&) {
      return makeUnexpected(UriFormatError::INVALID_URI_PORT);
    }
  }

  result.hasAuthority_ = true;
  result.host_ = host.str();
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
    // the parser
    return host_.substr(1, host_.size() - 2);
  }
  return host_;
}

const std::vector<std::pair<std::string, std::string>>& Uri::getQueryParams() {
  if (!query_.empty() && queryParams_.empty()) {
    // Finds successive matches of `(^|&)([^=&]*)=?([^=&]*)(?=&|$)` and skips
    // parameters without a name.
    StringPiece query(query_);
    size_t start = 0;
    while (start <= query.size()) {
      size_t begin = start;
      std::optional<QueryParam> param;
      if (isLineStart(query, start)) {
        param = matchQueryParam(query, begin);
        if (param && param->end == start) {
          // An empty match where the previous match ended doesn't count, so
          // try '&'.
          param.reset();
        }
      }
      if (!param && start < query.size() && query[start] == '&') {
        begin = start + 1;
        param = matchQueryParam(query, begin);
      }
      if (!param) {
        ++start;
        continue;
      }
      if (param->nameEnd > begin) {
        queryParams_.emplace_back(
            query.subpiece(begin, param->nameEnd - begin).str(),
            query.subpiece(param->valueBegin, param->end - param->valueBegin)
                .str());
      }
      start = param->end;
    }
  }
  return queryParams_;
}

} // namespace folly
