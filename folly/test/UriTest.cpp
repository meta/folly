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

#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include <folly/portability/GTest.h>

using namespace folly;
using namespace std::literals;

TEST(Uri, Simple) {
  {
    fbstring s("http://www.facebook.com/hello/world?query#fragment");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("www.facebook.com", u.host());
    EXPECT_EQ(0, u.port());
    EXPECT_EQ("www.facebook.com", u.authority());
    EXPECT_EQ("/hello/world", u.path());
    EXPECT_EQ("query", u.query());
    EXPECT_EQ("fragment", u.fragment());
    EXPECT_EQ(s, u.fbstr()); // canonical
  }

  {
    fbstring s("http://www.facebook.com:8080/hello/world?query#fragment");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("www.facebook.com", u.host());
    EXPECT_EQ(8080, u.port());
    EXPECT_EQ("www.facebook.com:8080", u.authority());
    EXPECT_EQ("/hello/world", u.path());
    EXPECT_EQ("query", u.query());
    EXPECT_EQ("fragment", u.fragment());
    EXPECT_EQ(s, u.fbstr()); // canonical
  }

  {
    fbstring s("http://127.0.0.1:8080/hello/world?query#fragment");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("127.0.0.1", u.host());
    EXPECT_EQ(8080, u.port());
    EXPECT_EQ("127.0.0.1:8080", u.authority());
    EXPECT_EQ("/hello/world", u.path());
    EXPECT_EQ("query", u.query());
    EXPECT_EQ("fragment", u.fragment());
    EXPECT_EQ(s, u.fbstr()); // canonical
  }

  {
    fbstring s("http://[::1]:8080/hello/world?query#fragment");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("[::1]", u.host());
    EXPECT_EQ("::1", u.hostname());
    EXPECT_EQ(8080, u.port());
    EXPECT_EQ("[::1]:8080", u.authority());
    EXPECT_EQ("/hello/world", u.path());
    EXPECT_EQ("query", u.query());
    EXPECT_EQ("fragment", u.fragment());
    EXPECT_EQ(s, u.fbstr()); // canonical
  }

  {
    fbstring s("http://[2401:db00:20:7004:face:0:29:0]:8080/hello/world?query");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("[2401:db00:20:7004:face:0:29:0]", u.host());
    EXPECT_EQ("2401:db00:20:7004:face:0:29:0", u.hostname());
    EXPECT_EQ(8080, u.port());
    EXPECT_EQ("[2401:db00:20:7004:face:0:29:0]:8080", u.authority());
    EXPECT_EQ("/hello/world", u.path());
    EXPECT_EQ("query", u.query());
    EXPECT_EQ("", u.fragment());
    EXPECT_EQ(s, u.fbstr()); // canonical
  }

  {
    fbstring s("http://[2401:db00:20:7004:face:0:29:0]/hello/world?query");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("[2401:db00:20:7004:face:0:29:0]", u.host());
    EXPECT_EQ("2401:db00:20:7004:face:0:29:0", u.hostname());
    EXPECT_EQ(0, u.port());
    EXPECT_EQ("[2401:db00:20:7004:face:0:29:0]", u.authority());
    EXPECT_EQ("/hello/world", u.path());
    EXPECT_EQ("query", u.query());
    EXPECT_EQ("", u.fragment());
    EXPECT_EQ(s, u.fbstr()); // canonical
  }

  {
    fbstring s("http://user:pass@host.com/");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("user", u.username());
    EXPECT_EQ("pass", u.password());
    EXPECT_EQ("host.com", u.host());
    EXPECT_EQ(0, u.port());
    EXPECT_EQ("user:pass@host.com", u.authority());
    EXPECT_EQ("/", u.path());
    EXPECT_EQ("", u.query());
    EXPECT_EQ("", u.fragment());
    EXPECT_EQ(s, u.fbstr());
  }

  {
    fbstring s("http://user@host.com/");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("user", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("host.com", u.host());
    EXPECT_EQ(0, u.port());
    EXPECT_EQ("user@host.com", u.authority());
    EXPECT_EQ("/", u.path());
    EXPECT_EQ("", u.query());
    EXPECT_EQ("", u.fragment());
    EXPECT_EQ(s, u.fbstr());
  }

  {
    fbstring s("http://user:@host.com/");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("user", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("host.com", u.host());
    EXPECT_EQ(0, u.port());
    EXPECT_EQ("user@host.com", u.authority());
    EXPECT_EQ("/", u.path());
    EXPECT_EQ("", u.query());
    EXPECT_EQ("", u.fragment());
    EXPECT_EQ("http://user@host.com/", u.fbstr());
  }

  {
    fbstring s("http://:pass@host.com/");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("pass", u.password());
    EXPECT_EQ("host.com", u.host());
    EXPECT_EQ(0, u.port());
    EXPECT_EQ(":pass@host.com", u.authority());
    EXPECT_EQ("/", u.path());
    EXPECT_EQ("", u.query());
    EXPECT_EQ("", u.fragment());
    EXPECT_EQ(s, u.fbstr());
  }

  {
    fbstring s("http://@host.com/");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("host.com", u.host());
    EXPECT_EQ(0, u.port());
    EXPECT_EQ("host.com", u.authority());
    EXPECT_EQ("/", u.path());
    EXPECT_EQ("", u.query());
    EXPECT_EQ("", u.fragment());
    EXPECT_EQ("http://host.com/", u.fbstr());
  }

  {
    fbstring s("http://:@host.com/");
    Uri u(s);
    EXPECT_EQ("http", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("host.com", u.host());
    EXPECT_EQ(0, u.port());
    EXPECT_EQ("host.com", u.authority());
    EXPECT_EQ("/", u.path());
    EXPECT_EQ("", u.query());
    EXPECT_EQ("", u.fragment());
    EXPECT_EQ("http://host.com/", u.fbstr());
  }

  {
    fbstring s("file:///etc/motd");
    Uri u(s);
    EXPECT_EQ("file", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("", u.host());
    EXPECT_EQ(0, u.port());
    EXPECT_EQ("", u.authority());
    EXPECT_EQ("/etc/motd", u.path());
    EXPECT_EQ("", u.query());
    EXPECT_EQ("", u.fragment());
    EXPECT_EQ(s, u.fbstr());
  }

  {
    fbstring s("file:/etc/motd");
    Uri u(s);
    EXPECT_EQ("file", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("", u.host());
    EXPECT_EQ(0, u.port());
    EXPECT_EQ("", u.authority());
    EXPECT_EQ("/etc/motd", u.path());
    EXPECT_EQ("", u.query());
    EXPECT_EQ("", u.fragment());
    EXPECT_EQ("file:/etc/motd", u.fbstr());
  }

  {
    fbstring s("file://etc/motd");
    Uri u(s);
    EXPECT_EQ("file", u.scheme());
    EXPECT_EQ("", u.username());
    EXPECT_EQ("", u.password());
    EXPECT_EQ("etc", u.host());
    EXPECT_EQ(0, u.port());
    EXPECT_EQ("etc", u.authority());
    EXPECT_EQ("/motd", u.path());
    EXPECT_EQ("", u.query());
    EXPECT_EQ("", u.fragment());
    EXPECT_EQ(s, u.fbstr());
  }

  {
    // test query parameters
    fbstring s("http://localhost?&key1=foo&key2=&key3&=bar&=bar=&");
    Uri u(s);
    auto paramsList = u.getQueryParams();
    std::map<fbstring, fbstring> params;
    for (auto& param : paramsList) {
      params[param.first] = param.second;
    }
    EXPECT_EQ(3, params.size());
    EXPECT_EQ("foo", params["key1"]);
    EXPECT_NE(params.end(), params.find("key2"));
    EXPECT_EQ("", params["key2"]);
    EXPECT_NE(params.end(), params.find("key3"));
    EXPECT_EQ("", params["key3"]);
  }

  {
    // test query parameters
    fbstring s("http://localhost?&&&&&&&&&&&&&&&");
    Uri u(s);
    auto params = u.getQueryParams();
    EXPECT_TRUE(params.empty());
  }

  {
    // test query parameters
    fbstring s("http://localhost?&=invalid_key&key2&key3=foo");
    Uri u(s);
    auto paramsList = u.getQueryParams();
    std::map<fbstring, fbstring> params;
    for (auto& param : paramsList) {
      params[param.first] = param.second;
    }
    EXPECT_EQ(2, params.size());
    EXPECT_NE(params.end(), params.find("key2"));
    EXPECT_EQ("", params["key2"]);
    EXPECT_EQ("foo", params["key3"]);
  }

  {
    // test query parameters
    fbstring s("http://localhost?&key1=====&&=key2&key3=");
    Uri u(s);
    auto paramsList = u.getQueryParams();
    std::map<fbstring, fbstring> params;
    for (auto& param : paramsList) {
      params[param.first] = param.second;
    }
    EXPECT_EQ(1, params.size());
    EXPECT_NE(params.end(), params.find("key3"));
    EXPECT_EQ("", params["key3"]);
  }

  {
    // test query parameters
    fbstring s("http://localhost?key1=foo=bar&key2=foobar&");
    Uri u(s);
    auto paramsList = u.getQueryParams();
    std::map<fbstring, fbstring> params;
    for (auto& param : paramsList) {
      params[param.first] = param.second;
    }
    EXPECT_EQ(1, params.size());
    EXPECT_EQ("foobar", params["key2"]);
  }

  {
    fbstring s("2http://www.facebook.com");

    try {
      Uri u(s);
      CHECK(false) << "Control should not have reached here";
    } catch (const std::invalid_argument& ex) {
      EXPECT_TRUE(std::string_view(ex.what()).ends_with(s));
    }
  }

  {
    fbstring s("www[facebook]com");

    try {
      Uri u("http://" + s);
      CHECK(false) << "Control should not have reached here";
    } catch (const std::invalid_argument& ex) {
      EXPECT_TRUE(std::string_view(ex.what()).ends_with(s));
    }
  }

  {
    fbstring s("http://[::1:8080/hello/world?query#fragment");

    try {
      Uri u(s);
      CHECK(false) << "Control should not have reached here";
    } catch (const std::invalid_argument&) {
      // success
    }
  }

  {
    fbstring s("http://::1]:8080/hello/world?query#fragment");

    try {
      Uri u(s);
      CHECK(false) << "Control should not have reached here";
    } catch (const std::invalid_argument&) {
      // success
    }
  }

  {
    fbstring s("http://::1:8080/hello/world?query#fragment");

    try {
      Uri u(s);
      CHECK(false) << "Control should not have reached here";
    } catch (const std::invalid_argument&) {
      // success
    }
  }

  {
    fbstring s("http://2401:db00:20:7004:face:0:29:0/hello/world?query");

    try {
      Uri u(s);
      CHECK(false) << "Control should not have reached here";
    } catch (const std::invalid_argument&) {
      // success
    }
  }

  {
    fbstring s("2http://www.facebook.com");

    auto u = Uri::tryFromString(s);
    EXPECT_TRUE(u.hasError());
    EXPECT_EQ(u.error(), UriFormatError::INVALID_URI);
  }

  // No authority (no "//") is valid
  {
    fbstring s("this:is/a/valid/uri");
    Uri u(s);
    EXPECT_EQ("this", u.scheme());
    EXPECT_EQ("is/a/valid/uri", u.path());
    EXPECT_EQ(s, u.fbstr());
  }
  {
    fbstring s("this:is:another:valid:uri");
    Uri u(s);
    EXPECT_EQ("this", u.scheme());
    EXPECT_EQ("is:another:valid:uri", u.path());
    EXPECT_EQ(s, u.fbstr());
  }
  {
    fbstring s("this:is@another:valid:uri");
    Uri u(s);
    EXPECT_EQ("this", u.scheme());
    EXPECT_EQ("is@another:valid:uri", u.path());
    EXPECT_EQ(s, u.fbstr());
  }
}

TEST(Uri, BadPortThrowsInvalidArgument) {
  constexpr std::string_view s = "http://localhost:9999999999999999999/";
  EXPECT_THROW(Uri{s}, std::invalid_argument);
}

TEST(Uri, Long) {
  std::string path(60000, 'p');
  std::string query;
  for (int i = 0; i < 5000; ++i) {
    toAppend("k", i, "=v&", &query);
  }
  query += "a=b=c";
  Uri u("http://host:80/" + path + "?" + query + "#f");
  EXPECT_EQ("/" + path, u.path());
  EXPECT_EQ(query, u.query());
  const auto& params = u.getQueryParams();
  ASSERT_EQ(5000, params.size());
  EXPECT_EQ("k4999", params.back().first);
  EXPECT_EQ("v", params.back().second);
}

namespace {

struct ParseCase {
  std::string_view uri = {};
  std::string_view scheme = {};
  std::string_view username = {};
  std::string_view password = {};
  std::string_view host = {};
  uint16_t port = 0;
  std::string_view path = {};
  std::string_view query = {};
  std::string_view fragment = {};
};

auto fields(const ParseCase& c) {
  return std::tuple(
      c.scheme,
      c.username,
      c.password,
      c.host,
      c.port,
      c.path,
      c.query,
      c.fragment);
}

auto fields(const Uri& u) {
  return std::tuple<
      std::string_view,
      std::string_view,
      std::string_view,
      std::string_view,
      uint16_t,
      std::string_view,
      std::string_view,
      std::string_view>(
      u.scheme(),
      u.username(),
      u.password(),
      u.host(),
      u.port(),
      u.path(),
      u.query(),
      u.fragment());
}

} // namespace

TEST(Uri, Parse) {
  const ParseCase cases[] = {
      // The scheme is a letter followed by letters, digits, '+', '-' and '.'.
      // It is lowercased.
      {.uri = "http://h/", .scheme = "http", .host = "h", .path = "/"},
      {.uri = "HTTP://h/", .scheme = "http", .host = "h", .path = "/"},
      {.uri = "Ab9+.-:x", .scheme = "ab9+.-", .path = "x"},
      {.uri = "a:", .scheme = "a"},

      // There is an authority only if "//" follows the scheme.
      {.uri = "s:/p", .scheme = "s", .path = "/p"},
      {.uri = "s:p//q", .scheme = "s", .path = "p//q"},
      {.uri = "s:?//q", .scheme = "s", .query = "//q"},
      {.uri = "s://", .scheme = "s"},
      {.uri = "s:///p", .scheme = "s", .path = "/p"},
      {.uri = "s://h", .scheme = "s", .host = "h"},

      // The userinfo ends at the first '@' and splits at its first ':'.
      {.uri = "s://u@h", .scheme = "s", .username = "u", .host = "h"},
      {.uri = "s://u:p@h",
       .scheme = "s",
       .username = "u",
       .password = "p",
       .host = "h"},
      {.uri = "s://u:@h", .scheme = "s", .username = "u", .host = "h"},
      {.uri = "s://:p@h", .scheme = "s", .password = "p", .host = "h"},
      {.uri = "s://@h", .scheme = "s", .host = "h"},
      {.uri = "s://:@h", .scheme = "s", .host = "h"},
      {.uri = "s://u:p:q@h",
       .scheme = "s",
       .username = "u",
       .password = "p:q",
       .host = "h"},
      {.uri = "s://u@", .scheme = "s", .username = "u"},
      {.uri = "s://a:b@c@d:1",
       .scheme = "s",
       .username = "a",
       .password = "b",
       .host = "c@d",
       .port = 1},
      {.uri = "s://@@h", .scheme = "s", .host = "@h"},
      {.uri = "s://u@[::1]:80",
       .scheme = "s",
       .username = "u",
       .host = "[::1]",
       .port = 80},
      // Without a valid host and port after the '@', the whole authority is
      // the host.
      {.uri = "s://[a@b:c]", .scheme = "s", .host = "[a@b:c]"},
      {.uri = "s://[a@b[c]:1", .scheme = "s", .host = "[a@b[c]", .port = 1},

      // A host is an IP literal in brackets or a run of characters other than
      // '[' and ':'.
      {.uri = "s://127.0.0.1:8080",
       .scheme = "s",
       .host = "127.0.0.1",
       .port = 8080},
      {.uri = "s://[::1]", .scheme = "s", .host = "[::1]"},
      {.uri = "s://[2401:db00::1]:443/x",
       .scheme = "s",
       .host = "[2401:db00::1]",
       .port = 443,
       .path = "/x"},
      {.uri = "s://[]", .scheme = "s", .host = "[]"},
      {.uri = "s://[v1.x:y@z]",
       .scheme = "s",
       .username = "[v1.x",
       .password = "y",
       .host = "z]"},
      {.uri = "s://h]", .scheme = "s", .host = "h]"},
      {.uri = "s://ex%41mple.com", .scheme = "s", .host = "ex%41mple.com"},
      {.uri = "s://:", .scheme = "s"},

      // The port is a possibly empty run of digits.
      {.uri = "s://h:", .scheme = "s", .host = "h"},
      {.uri = "s://h:0", .scheme = "s", .host = "h"},
      {.uri = "s://h:080", .scheme = "s", .host = "h", .port = 80},
      {.uri = "s://h:65535", .scheme = "s", .host = "h", .port = 65535},
      {.uri = "s://h:00000000000000000065535",
       .scheme = "s",
       .host = "h",
       .port = 65535},

      // The fragment follows the first '#', and the query follows the first
      // '?' before it.
      {.uri = "s:?", .scheme = "s"},
      {.uri = "s:#", .scheme = "s"},
      {.uri = "s:#?", .scheme = "s", .fragment = "?"},
      {.uri = "s:p?q#f",
       .scheme = "s",
       .path = "p",
       .query = "q",
       .fragment = "f"},
      {.uri = "s:p#f?q", .scheme = "s", .path = "p", .fragment = "f?q"},
      {.uri = "s:p?q?r#f#g",
       .scheme = "s",
       .path = "p",
       .query = "q?r",
       .fragment = "f#g"},
      {.uri = "s://h?q", .scheme = "s", .host = "h", .query = "q"},
      {.uri = "s://h#f", .scheme = "s", .host = "h", .fragment = "f"},
      {.uri = "s://h/p/../q;x=1",
       .scheme = "s",
       .host = "h",
       .path = "/p/../q;x=1"},

      // Percent signs and other bytes are kept as is.
      {.uri = "s://h/a%2Fb?c%3Dd#e%23",
       .scheme = "s",
       .host = "h",
       .path = "/a%2Fb",
       .query = "c%3Dd",
       .fragment = "e%23"},
      {.uri = "s://h/a b\n?c\r#d\0e"sv,
       .scheme = "s",
       .host = "h",
       .path = "/a b\n",
       .query = "c\r",
       .fragment = "d\0e"sv},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.uri);
    EXPECT_EQ(fields(c), fields(Uri(c.uri)));
  }
}

TEST(Uri, Errors) {
  const std::pair<std::string_view, UriFormatError> cases[] = {
      {"", UriFormatError::INVALID_URI},
      {":", UriFormatError::INVALID_URI},
      {"http", UriFormatError::INVALID_URI},
      {"//h/p", UriFormatError::INVALID_URI},
      {"1http:x", UriFormatError::INVALID_URI},
      {"+a:x", UriFormatError::INVALID_URI},
      {"ht tp:x", UriFormatError::INVALID_URI},
      {"ht_tp:x", UriFormatError::INVALID_URI},
      {"h%74tp:x", UriFormatError::INVALID_URI},

      {"s://[::1", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://[::1]x", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://[::1]]", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://::1", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://::1]:80", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://h[x]", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://h:1:2", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://h:-1", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://h:+1", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://h:1a", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://h: 1", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://u:p@h:x", UriFormatError::INVALID_URI_AUTHORITY},
      {"s://a@b:c@d", UriFormatError::INVALID_URI_AUTHORITY},

      {"s://h:65536", UriFormatError::INVALID_URI_PORT},
      {"s://h:99999999999999999999", UriFormatError::INVALID_URI_PORT},
      {"s://u:p@h:70000/x", UriFormatError::INVALID_URI_PORT},
  };
  for (auto [uri, error] : cases) {
    SCOPED_TRACE(uri);
    auto result = Uri::tryFromString(uri);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(error, result.error());
    std::string message = error == UriFormatError::INVALID_URI ? "invalid URI: "
        : error == UriFormatError::INVALID_URI_AUTHORITY
        ? "invalid URI authority: "
        : "invalid URI port: ";
    message += uri;
    try {
      Uri u(uri);
      ADD_FAILURE();
    } catch (const std::invalid_argument& e) {
      EXPECT_EQ(message, e.what());
    }
  }
}

TEST(Uri, Print) {
  struct PrintCase {
    std::string_view uri;
    std::string_view str;
    std::string_view authority;
    std::string_view hostname;
  };
  const PrintCase cases[] = {
      {"HTTP://U:P@H:080/p?q#f", "http://U:P@H:80/p?q#f", "U:P@H:80", "H"},
      {"s://u:@h/", "s://u@h/", "u@h", "h"},
      {"s://:p@h", "s://:p@h", ":p@h", "h"},
      {"s://@h:0/", "s://h/", "h", "h"},
      {"s://@@h", "s://@h", "@h", "@h"},
      {"s://h?#", "s://h", "h", "h"},
      {"s:p?#", "s:p", "", ""},
      {"s://", "s://", "", ""},
      {"s://[::1]:1", "s://[::1]:1", "[::1]:1", "::1"},
      {"s://[]", "s://[]", "[]", ""},
      {"s://[a@b:c]", "s://[a@b:c]", "[a@b:c]", "a@b:c"},
  };
  for (const auto& c : cases) {
    SCOPED_TRACE(c.uri);
    Uri u(c.uri);
    EXPECT_EQ(
        std::tuple(c.str, c.authority, c.hostname),
        std::tuple(u.str(), u.authority(), u.hostname()));
  }
}

TEST(Uri, QueryParams) {
  using Params = std::vector<std::pair<std::string, std::string>>;
  const std::pair<std::string_view, Params> cases[] = {
      {"a=b", {{"a", "b"}}},
      {"a", {{"a", ""}}},
      {"a=", {{"a", ""}}},
      {"a=b&c=d", {{"a", "b"}, {"c", "d"}}},
      {"a&b", {{"a", ""}, {"b", ""}}},
      {"a=b&&c=d&", {{"a", "b"}, {"c", "d"}}},
      {"&&&", {}},
      // Parameters without a name or with more than one '=' are skipped.
      {"=b", {}},
      {"=", {}},
      {"a=b=c", {}},
      {"a==b", {}},
      {"a=b=c&d=e", {{"d", "e"}}},
      {"a%3D=b%26+c", {{"a%3D", "b%26+c"}}},
      // A parameter can also start after and end before CR, LF or FF, but
      // not between CR and LF, and its name and value can span them.
      {"a=b\nc=d", {{"a", "b"}, {"c", "d"}}},
      {"a=b\rc=d", {{"a", "b"}, {"c", "d"}}},
      {"a=b\fc=d", {{"a", "b"}, {"c", "d"}}},
      {"a=b\r\nc=d", {{"a", "b"}, {"c", "d"}}},
      {"a=b=c\nd=e", {{"d", "e"}}},
      {"a=b\r\nc=d=e", {{"a", "b"}}},
      {"a\nb=c", {{"a\nb", "c"}}},
      {"a=b\nc", {{"a", "b\nc"}}},
      {"ab\ncd=e=f", {{"ab", ""}}},
      {"\na=b=c", {}},
  };
  for (const auto& [query, params] : cases) {
    SCOPED_TRACE(query);
    Uri u(std::string("s:?").append(query));
    EXPECT_EQ(params, u.getQueryParams());
  }
}
