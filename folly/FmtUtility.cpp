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

#include <folly/FmtUtility.h>

#include <folly/Range.h>
#include <folly/String.h>
#include <folly/ssl/OpenSSLHash.h>

namespace folly {

std::string fmt_vformat_mangle_name_fn::operator()(
    std::string_view const key) const {
  auto& self = *this;
  std::string out;
  self(out, key);
  return out;
}

void fmt_vformat_mangle_name_fn::operator()(
    std::string& out, std::string_view const key) const {
  auto const keyr = folly::ByteRange(key);
  uint8_t enc[32];
  auto const encr = folly::MutableByteRange{std::begin(enc), std::end(enc)};
#if FOLLY_OPENSSL_HAS_BLAKE2B
  folly::ssl::OpenSSLHash::blake2s256(encr, keyr);
#else
  folly::ssl::OpenSSLHash::sha256(encr, keyr);
#endif
  out.push_back('_');
  folly::hexlify(encr, out, true);
}

std::string fmt_vformat_mangle_format_string_fn::operator()(
    std::string_view const str) const {
  return operator()(options{}, str);
}

std::string fmt_vformat_mangle_format_string_fn::operator()(
    options const& opts, std::string_view const str) const {
  auto const fe_opts =
      format_string_for_each_named_arg_options{} //
          .set_numeric_args_as_named(opts.numeric_args_as_named);
  std::string out;
  char const* pos = str.data();
  format_string_for_each_named_arg(fe_opts, str, [&](auto const arg) {
    out.append(pos, arg.data());
    fmt_vformat_mangle_name(out, arg);
    pos = arg.data() + arg.size();
  });
  out.append(pos, str.data() + str.size());
  return out;
}

} // namespace folly
