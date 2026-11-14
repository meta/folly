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

#include <folly/ssl/OpenSSLCertUtils.h>

#include <optional>

#include <folly/FileUtil.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/ssl/OpenSSLPtrTypes.h>

namespace folly {
namespace ssl {

namespace {
std::string getOpenSSLErrorString(unsigned long err) {
  std::array<char, 256> errBuff;
  ERR_error_string_n(err, errBuff.data(), errBuff.size());
  return std::string(errBuff.data());
}

std::string asn1ToString(const ASN1_STRING* a) {
  auto strType = ASN1_STRING_type(a);
  if (strType == V_ASN1_UTF8STRING || strType == V_ASN1_OCTET_STRING) {
    long len = ASN1_STRING_length(a);
    int type, xclass;
    const unsigned char* data = ASN1_STRING_get0_data(a);
    ASN1_get_object(&data, &len, &type, &xclass, len);
    return std::string(reinterpret_cast<const char*>(data), len);
  } else {
    unsigned char* data = const_cast<unsigned char*>(ASN1_STRING_get0_data(a));
    int len = ASN1_STRING_length(a);
    if (len <= 0) {
      return std::string();
    }
    return std::string(reinterpret_cast<char*>(data), len);
  }
}

std::optional<std::string> getStringSubjectAltName(const ASN1_STRING* value) {
  const auto* data =
      reinterpret_cast<const char*>(ASN1_STRING_get0_data(value));
  const int len = ASN1_STRING_length(value);
  if (!data || len <= 0) {
    return std::nullopt;
  }
  return std::string(data, len);
}

std::optional<IPAddress> getIpSubjectAltName(const ASN1_STRING* value) {
  const auto* data = ASN1_STRING_get0_data(value);
  const int len = ASN1_STRING_length(value);
  if (!data || (len != 4 && len != 16)) {
    return std::nullopt;
  }
  return IPAddress::fromBinary(ByteRange(data, static_cast<size_t>(len)));
}

std::optional<GeneralName> getSubjectAltName(const GENERAL_NAME& name) {
  if (name.type == GEN_DNS) {
    if (auto value = getStringSubjectAltName(name.d.dNSName)) {
      return GeneralName{DnsName{std::move(*value)}};
    }
  }
  if (name.type == GEN_URI) {
    if (auto value =
            getStringSubjectAltName(name.d.uniformResourceIdentifier)) {
      return GeneralName{UriName{std::move(*value)}};
    }
  }
  if (name.type == GEN_IPADD) {
    if (auto value = getIpSubjectAltName(name.d.iPAddress)) {
      return GeneralName{std::move(*value)};
    }
  }
  return std::nullopt;
}

std::string getExtOid(const X509_EXTENSION* extension) {
  CHECK_NOTNULL(extension);
  // X509_EXTENSION_get_object() takes a non-const parameter before
  // OpenSSL 4.0; the cast is a no-op there.
  const ASN1_OBJECT* object =
      X509_EXTENSION_get_object(const_cast<X509_EXTENSION*>(extension));
  // Query for extension OID
  constexpr int buf_size = 256;
  std::string ret(buf_size, '\0');
  auto length = OBJ_obj2txt(ret.data(), ret.size(), object, 1);

  if (length < 0) {
    return {};
  }

  if (length >= buf_size) {
    // Reserve one byte for the terminating zero
    ret.resize(static_cast<size_t>(length) + 1, '\0');
    OBJ_obj2txt(ret.data(), ret.size(), object, 1);
  }
  ret.resize(static_cast<size_t>(length));
  return ret;
}

std::string getExtData(const X509_EXTENSION* extension) {
  CHECK_NOTNULL(extension);
  // X509_EXTENSION_get_data() takes a non-const parameter before
  // OpenSSL 4.0; the cast is a no-op there.
  auto asnValue =
      X509_EXTENSION_get_data(const_cast<X509_EXTENSION*>(extension));
  return asnValue ? asn1ToString(asnValue) : std::string();
}

Optional<std::string> commonName(const X509_NAME* name) {
  if (!name) {
    return none;
  }

  // X509_NAME_get_index_by_NID() takes a non-const parameter before
  // OpenSSL 3.0; the cast is a no-op there.
  auto cnLoc = X509_NAME_get_index_by_NID(
      const_cast<X509_NAME*>(name), NID_commonName, -1);
  if (cnLoc < 0) {
    return none;
  }

  auto cnEntry = X509_NAME_get_entry(name, cnLoc);
  if (!cnEntry) {
    return none;
  }

  auto cnAsn = X509_NAME_ENTRY_get_data(cnEntry);
  if (!cnAsn) {
    return none;
  }

  auto cnData = reinterpret_cast<const char*>(ASN1_STRING_get0_data(cnAsn));
  auto cnLen = ASN1_STRING_length(cnAsn);
  if (!cnData || cnLen <= 0) {
    return none;
  }

  return Optional<std::string>(std::string(cnData, cnLen));
}

} // namespace

Optional<std::string> OpenSSLCertUtils::getCommonName(const X509& x509) {
  return commonName(X509_get_subject_name(&x509));
}

Optional<std::string> OpenSSLCertUtils::getIssuerCommonName(const X509& x509) {
  return commonName(X509_get_issuer_name(&x509));
}

std::vector<std::string> OpenSSLCertUtils::getSubjectAltNames(
    const X509& x509) {
  std::vector<std::string> result;
  for (auto& name : getSubjectAltNameEntries(x509)) {
    if (auto* dnsName = std::get_if<DnsName>(&name)) {
      result.emplace_back(std::move(dnsName->value));
    }
  }
  return result;
}

std::vector<std::string> OpenSSLCertUtils::getSubjectAltNameURIs(
    const X509& x509) {
  std::vector<std::string> result;
  for (auto& name : getSubjectAltNameEntries(x509)) {
    if (auto* uriName = std::get_if<UriName>(&name)) {
      result.emplace_back(std::move(uriName->value));
    }
  }
  return result;
}

std::vector<GeneralName> OpenSSLCertUtils::getSubjectAltNameEntries(
    const X509& x509) {
  auto names = reinterpret_cast<STACK_OF(GENERAL_NAME)*>(
      X509_get_ext_d2i(&x509, NID_subject_alt_name, nullptr, nullptr));
  if (!names) {
    return {};
  }
  SCOPE_EXIT {
    sk_GENERAL_NAME_pop_free(names, GENERAL_NAME_free);
  };

  auto count = sk_GENERAL_NAME_num(names);
  std::vector<GeneralName> result;
  result.reserve(count);
  for (int i = 0; i < count; i++) {
    auto name = sk_GENERAL_NAME_value(names, i);
    if (!name) {
      continue;
    }
    if (auto value = getSubjectAltName(*name)) {
      result.emplace_back(std::move(*value));
    }
  }
  return result;
}

std::vector<std::string> OpenSSLCertUtils::getExtendedKeyUsage(
    const X509& x509) {
  auto ekuExtension = reinterpret_cast<EXTENDED_KEY_USAGE*>(
      X509_get_ext_d2i(&x509, NID_ext_key_usage, nullptr, nullptr));
  if (!ekuExtension) {
    return {};
  }
  SCOPE_EXIT {
    EXTENDED_KEY_USAGE_free(ekuExtension);
  };

  std::vector<std::string> ekuOids;
  for (int i = 0; i < sk_ASN1_OBJECT_num(ekuExtension); i++) {
    ASN1_OBJECT* obj = sk_ASN1_OBJECT_value(ekuExtension, i);
    // The max legitimate OID size is 586 bytes.
    // (https://github.com/openssl/openssl/blob/f5408861fa93e44e2d6a1ba829ea43ca8be01717/crypto/objects/obj_dat.c#L436-L437)
    constexpr size_t kBufSize = 1024;
    std::array<char, kBufSize> buf{};
    auto len = OBJ_obj2txt(buf.data(), buf.size(), obj, /*no_name=*/1);

    if (len < 0) {
      throw std::runtime_error("failed to convert oid to string");
    }

    if (len + 1 > static_cast<int>(kBufSize)) {
      // This can never happen with a legitimate OID.
      throw std::runtime_error("illegal oid");
    }
    ekuOids.push_back(std::string(buf.data(), len));
  }
  return ekuOids;
}

Optional<std::string> OpenSSLCertUtils::getSubject(const X509& x509) {
  auto subject = X509_get_subject_name(&x509);
  if (!subject) {
    return none;
  }

  auto bio = BioUniquePtr(BIO_new(BIO_s_mem()));
  if (bio == nullptr) {
    throw std::runtime_error("Cannot allocate bio");
  }
  if (X509_NAME_print_ex(bio.get(), subject, 0, XN_FLAG_ONELINE) <= 0) {
    return none;
  }

  char* bioData = nullptr;
  size_t bioLen = BIO_get_mem_data(bio.get(), &bioData);
  return std::string(bioData, bioLen);
}

Optional<std::string> OpenSSLCertUtils::getIssuer(const X509& x509) {
  auto issuer = X509_get_issuer_name(&x509);
  if (!issuer) {
    return none;
  }

  auto bio = BioUniquePtr(BIO_new(BIO_s_mem()));
  if (bio == nullptr) {
    throw std::runtime_error("Cannot allocate bio");
  }

  if (X509_NAME_print_ex(bio.get(), issuer, 0, XN_FLAG_ONELINE) <= 0) {
    return none;
  }

  char* bioData = nullptr;
  size_t bioLen = BIO_get_mem_data(bio.get(), &bioData);
  return std::string(bioData, bioLen);
}

std::vector<std::string> OpenSSLCertUtils::getExtension(
    const X509& x509, folly::StringPiece oid) {
  std::vector<std::string> extValues;
  for (int i = 0; i < X509_get_ext_count(&x509); i++) {
    const X509_EXTENSION* extension = X509_get_ext(&x509, i);
    std::string extensionOid = getExtOid(extension);
    if (extensionOid == oid) {
      extValues.push_back(getExtData(extension));
    }
  }
  return extValues;
}

std::vector<std::pair<std::string, std::string>>
OpenSSLCertUtils::getAllExtensions(const X509& x509) {
  std::vector<std::pair<std::string, std::string>> extensions;
  for (int i = 0; i < X509_get_ext_count(&x509); i++) {
    const X509_EXTENSION* extension = X509_get_ext(&x509, i);
    std::string oid = getExtOid(extension);
    std::string value = getExtData(extension);
    extensions.push_back(std::make_pair(oid, value));
  }
  return extensions;
}

folly::Optional<std::string> OpenSSLCertUtils::toString(X509& x509) {
  auto in = BioUniquePtr(BIO_new(BIO_s_mem()));
  if (in == nullptr) {
    throw std::runtime_error("Cannot allocate bio");
  }

  int flags = 0;

  flags |= X509_FLAG_NO_HEADER | /* A few bytes of cert and data */
      X509_FLAG_NO_PUBKEY | /* Public key */
      X509_FLAG_NO_AUX | /* Auxiliary info? */
      X509_FLAG_NO_SIGDUMP | /* Prints the signature */
      X509_FLAG_NO_SIGNAME; /* Signature algorithms */

#ifdef X509_FLAG_NO_IDS
  flags |= X509_FLAG_NO_IDS; /* Issuer/subject IDs */
#endif

  if (X509_print_ex(in.get(), &x509, XN_FLAG_ONELINE, flags) > 0) {
    char* bioData = nullptr;
    size_t bioLen = BIO_get_mem_data(in.get(), &bioData);
    return std::string(bioData, bioLen);
  } else {
    return none;
  }
}

std::string OpenSSLCertUtils::getNotAfterTime(const X509& x509) {
  return getDateTimeStr(X509_get0_notAfter(&x509));
}

std::string OpenSSLCertUtils::getNotBeforeTime(const X509& x509) {
  return getDateTimeStr(X509_get0_notBefore(&x509));
}

std::chrono::system_clock::time_point OpenSSLCertUtils::asnTimeToTimepoint(
    const ASN1_TIME* asnTime) {
  int dSecs = 0;
  int dDays = 0;

  auto epoch_time_t = std::chrono::system_clock::to_time_t(
      std::chrono::system_clock::time_point());
  folly::ssl::ASN1TimeUniquePtr epoch_asn(ASN1_TIME_set(nullptr, epoch_time_t));

  if (!epoch_asn) {
    throw std::runtime_error("failed to allocate epoch asn.1 time");
  }

  if (ASN1_TIME_diff(&dDays, &dSecs, epoch_asn.get(), asnTime) != 1) {
    throw std::runtime_error("invalid asn.1 time");
  }

  return std::chrono::system_clock::time_point(
      std::chrono::seconds(dSecs) + std::chrono::hours(24 * dDays));
}

std::string OpenSSLCertUtils::getDateTimeStr(const ASN1_TIME* time) {
  if (!time) {
    return "";
  }

  auto bio = BioUniquePtr(BIO_new(BIO_s_mem()));
  if (bio == nullptr) {
    throw std::runtime_error("Cannot allocate bio");
  }

  if (ASN1_TIME_print(bio.get(), time) <= 0) {
    throw std::runtime_error("Cannot print ASN1_TIME");
  }

  char* bioData = nullptr;
  size_t bioLen = BIO_get_mem_data(bio.get(), &bioData);
  return std::string(bioData, bioLen);
}

X509UniquePtr OpenSSLCertUtils::derDecode(ByteRange range) {
  auto begin = range.data();
  X509UniquePtr cert(d2i_X509(nullptr, &begin, range.size()));
  if (!cert) {
    throw std::runtime_error("could not read cert");
  }
  return cert;
}

std::unique_ptr<IOBuf> OpenSSLCertUtils::derEncode(X509& x509) {
  auto len = i2d_X509(&x509, nullptr);
  if (len < 0) {
    throw std::runtime_error("Error computing length");
  }
  auto buf = IOBuf::create(len);
  auto dataPtr = buf->writableData();
  len = i2d_X509(&x509, &dataPtr);
  if (len < 0) {
    throw std::runtime_error("Error converting cert to DER");
  }
  buf->append(len);
  return buf;
}

X509UniquePtr OpenSSLCertUtils::pemDecode(ByteRange range) {
  BioUniquePtr bio(BIO_new_mem_buf(range.data(), range.size()));
  if (!bio) {
    throw std::runtime_error("failed to create BIO");
  }
  X509UniquePtr x509(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
  if (!x509) {
    throw std::runtime_error("failed to read cert");
  }
  return x509;
}

std::string OpenSSLCertUtils::pemEncode(X509& x509) {
  BioUniquePtr bio(BIO_new(BIO_s_mem()));
  if (!PEM_write_bio_X509(bio.get(), &x509)) {
    throw std::runtime_error("Failed to encode X509");
  }
  BUF_MEM* bptr = nullptr;
  BIO_get_mem_ptr(bio.get(), &bptr);
  return {bptr->data, bptr->length};
}

std::vector<X509UniquePtr> OpenSSLCertUtils::readCertsFromBuffer(
    ByteRange range) {
  BioUniquePtr b(
      BIO_new_mem_buf(const_cast<unsigned char*>(range.data()), range.size()));
  if (!b) {
    throw std::runtime_error("failed to create BIO");
  }
  std::vector<X509UniquePtr> certs;
  ERR_clear_error();
  while (true) {
    X509UniquePtr x509(PEM_read_bio_X509(b.get(), nullptr, nullptr, nullptr));
    if (x509) {
      certs.push_back(std::move(x509));
      continue;
    }
    auto err = ERR_get_error();
    ERR_clear_error();
    if (BIO_eof(b.get()) && ERR_GET_LIB(err) == ERR_LIB_PEM &&
        ERR_GET_REASON(err) == PEM_R_NO_START_LINE) {
      // Reach end of buffer.
      break;
    }
    throw std::runtime_error(
        folly::to<std::string>(
            "Unable to parse cert ",
            certs.size(),
            ": ",
            getOpenSSLErrorString(err)));
  }
  return certs;
}

std::array<uint8_t, SHA_DIGEST_LENGTH> OpenSSLCertUtils::getDigestSha1(
    const X509& x509) {
  unsigned int len;
  std::array<uint8_t, SHA_DIGEST_LENGTH> md;
  int rc = X509_digest(&x509, EVP_sha1(), md.data(), &len);

  if (rc <= 0) {
    throw std::runtime_error("Could not calculate SHA1 digest for cert");
  }
  return md;
}

std::array<uint8_t, SHA256_DIGEST_LENGTH> OpenSSLCertUtils::getDigestSha256(
    const X509& x509) {
  unsigned int len;
  std::array<uint8_t, SHA256_DIGEST_LENGTH> md;
  int rc = X509_digest(&x509, EVP_sha256(), md.data(), &len);

  if (rc <= 0) {
    throw std::runtime_error("Could not calculate SHA256 digest for cert");
  }
  return md;
}

X509StoreUniquePtr OpenSSLCertUtils::readStoreFromFile(std::string caFile) {
  std::string certData;
  if (!folly::readFile(caFile.c_str(), certData)) {
    throw std::runtime_error(
        folly::to<std::string>("Could not read store file: ", caFile));
  }
  return readStoreFromBuffer(folly::StringPiece(certData));
}

X509StoreUniquePtr OpenSSLCertUtils::readStoreFromBuffer(ByteRange certRange) {
  auto certs = readCertsFromBuffer(certRange);
  ERR_clear_error();
  folly::ssl::X509StoreUniquePtr store(X509_STORE_new());
  for (auto& caCert : certs) {
    if (X509_STORE_add_cert(store.get(), caCert.get()) != 1) {
      auto err = ERR_get_error();
      if (ERR_GET_LIB(err) != ERR_LIB_X509 ||
          ERR_GET_REASON(err) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
        throw std::runtime_error(
            folly::to<std::string>(
                "Could not insert CA certificate into store: ",
                getOpenSSLErrorString(err)));
      }
    }
  }
  return store;
}
} // namespace ssl
} // namespace folly