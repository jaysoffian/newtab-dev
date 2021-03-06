/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ssl.h"
#include "sslproto.h"

#include <memory>

#include "tls_parser.h"
#include "tls_filter.h"
#include "tls_connect.h"

namespace nss_test {

class TlsExtensionFilter : public TlsHandshakeFilter {
 protected:
  virtual PacketFilter::Action FilterHandshake(
      const HandshakeHeader& header,
      const DataBuffer& input, DataBuffer* output) {
    if (header.handshake_type() == kTlsHandshakeClientHello) {
      TlsParser parser(input);
      if (!FindClientHelloExtensions(&parser, header)) {
        return KEEP;
      }
      return FilterExtensions(&parser, input, output);
    }
    if (header.handshake_type() == kTlsHandshakeServerHello) {
      TlsParser parser(input);
      if (!FindServerHelloExtensions(&parser, header.version())) {
        return KEEP;
      }
      return FilterExtensions(&parser, input, output);
    }
    return KEEP;
  }

  virtual PacketFilter::Action FilterExtension(uint16_t extension_type,
                                               const DataBuffer& input,
                                               DataBuffer* output) = 0;

 public:
  static bool FindClientHelloExtensions(TlsParser* parser, const Versioned& header) {
    if (!parser->Skip(2 + 32)) { // version + random
      return false;
    }
    if (!parser->SkipVariable(1)) { // session ID
      return false;
    }
    if (header.is_dtls() && !parser->SkipVariable(1)) { // DTLS cookie
      return false;
    }
    if (!parser->SkipVariable(2)) { // cipher suites
      return false;
    }
    if (!parser->SkipVariable(1)) { // compression methods
      return false;
    }
    return true;
  }

  static bool FindServerHelloExtensions(TlsParser* parser, uint16_t version) {
    if (!parser->Skip(2 + 32)) { // version + random
      return false;
    }
    if (!parser->SkipVariable(1)) { // session ID
      return false;
    }
    if (!parser->Skip(2)) { // cipher suite
      return false;
    }
    if (NormalizeTlsVersion(version) <= SSL_LIBRARY_VERSION_TLS_1_2) {
      if (!parser->Skip(1)) { // compression method
        return false;
      }
    }
    return true;
  }

 private:
  PacketFilter::Action FilterExtensions(TlsParser* parser,
                                        const DataBuffer& input,
                                        DataBuffer* output) {
    size_t length_offset = parser->consumed();
    uint32_t all_extensions;
    if (!parser->Read(&all_extensions, 2)) {
      return KEEP; // no extensions, odd but OK
    }
    if (all_extensions != parser->remaining()) {
      return KEEP; // malformed
    }

    bool changed = false;

    // Write out the start of the message.
    output->Allocate(input.len());
    size_t offset = output->Write(0, input.data(), parser->consumed());

    while (parser->remaining()) {
      uint32_t extension_type;
      if (!parser->Read(&extension_type, 2)) {
        return KEEP; // malformed
      }

      DataBuffer extension;
      if (!parser->ReadVariable(&extension, 2)) {
        return KEEP; // malformed
      }

      DataBuffer filtered;
      PacketFilter::Action action = FilterExtension(extension_type, extension,
                                                    &filtered);
      if (action == DROP) {
        changed = true;
        std::cerr << "extension drop: " << extension << std::endl;
        continue;
      }

      const DataBuffer* source = &extension;
      if (action == CHANGE) {
        EXPECT_GT(0x10000U, filtered.len());
        changed = true;
        std::cerr << "extension old: " << extension << std::endl;
        std::cerr << "extension new: " << filtered << std::endl;
        source = &filtered;
      }

      // Write out extension.
      offset = output->Write(offset, extension_type, 2);
      offset = output->Write(offset, source->len(), 2);
      offset = output->Write(offset, *source);
    }
    output->Truncate(offset);

    if (changed) {
      size_t newlen = output->len() - length_offset - 2;
      EXPECT_GT(0x10000U, newlen);
      if (newlen >= 0x10000) {
        return KEEP; // bad: size increased too much
      }
      output->Write(length_offset, newlen, 2);
      return CHANGE;
    }
    return KEEP;
  }
};

class TlsExtensionTruncator : public TlsExtensionFilter {
 public:
  TlsExtensionTruncator(uint16_t extension, size_t length)
      : extension_(extension), length_(length) {}
  virtual PacketFilter::Action FilterExtension(
      uint16_t extension_type, const DataBuffer& input, DataBuffer* output) {
    if (extension_type != extension_) {
      return KEEP;
    }
    if (input.len() <= length_) {
      return KEEP;
    }

    output->Assign(input.data(), length_);
    return CHANGE;
  }
 private:
    uint16_t extension_;
    size_t length_;
};

class TlsExtensionDamager : public TlsExtensionFilter {
 public:
  TlsExtensionDamager(uint16_t extension, size_t index)
      : extension_(extension), index_(index) {}
  virtual PacketFilter::Action FilterExtension(
      uint16_t extension_type, const DataBuffer& input, DataBuffer* output) {
    if (extension_type != extension_) {
      return KEEP;
    }

    *output = input;
    output->data()[index_] += 73; // Increment selected for maximum damage
    return CHANGE;
  }
 private:
  uint16_t extension_;
  size_t index_;
};

class TlsExtensionReplacer : public TlsExtensionFilter {
 public:
  TlsExtensionReplacer(uint16_t extension, const DataBuffer& data)
      : extension_(extension), data_(data) {}
  virtual PacketFilter::Action FilterExtension(
      uint16_t extension_type, const DataBuffer& input, DataBuffer* output) {
    if (extension_type != extension_) {
      return KEEP;
    }

    *output = data_;
    return CHANGE;
  }
 private:
  const uint16_t extension_;
  const DataBuffer data_;
};

class TlsExtensionInjector : public TlsHandshakeFilter {
 public:
  TlsExtensionInjector(uint16_t ext, DataBuffer& data)
      : extension_(ext), data_(data) {}

  virtual PacketFilter::Action FilterHandshake(
      const HandshakeHeader& header,
      const DataBuffer& input, DataBuffer* output) {
    size_t offset;
    if (header.handshake_type() == kTlsHandshakeClientHello) {
      TlsParser parser(input);
      if (!TlsExtensionFilter::FindClientHelloExtensions(&parser, header)) {
        return KEEP;
      }
      offset = parser.consumed();
    } else if (header.handshake_type() == kTlsHandshakeServerHello) {
      TlsParser parser(input);
      if (!TlsExtensionFilter::FindServerHelloExtensions(&parser, header.version())) {
        return KEEP;
      }
      offset = parser.consumed();
    } else {
      return KEEP;
    }

    *output = input;

    // Increase the size of the extensions.
    uint16_t* len_addr = reinterpret_cast<uint16_t*>(output->data() + offset);
    *len_addr = htons(ntohs(*len_addr) + data_.len() + 4);

    // Insert the extension type and length.
    DataBuffer type_length;
    type_length.Allocate(4);
    type_length.Write(0, extension_, 2);
    type_length.Write(2, data_.len(), 2);
    output->Splice(type_length, offset + 2);

    // Insert the payload.
    output->Splice(data_, offset + 6);

    return CHANGE;
  }

 private:
  const uint16_t extension_;
  const DataBuffer data_;
};

class TlsExtensionCapture : public TlsExtensionFilter {
 public:
  TlsExtensionCapture(uint16_t ext)
      : extension_(ext), data_() {}

  virtual PacketFilter::Action FilterExtension(
      uint16_t extension_type, const DataBuffer& input, DataBuffer* output) {
    if (extension_type == extension_) {
      data_.Assign(input);
    }
    return KEEP;
  }

  const DataBuffer& extension() const { return data_; }

 private:
  const uint16_t extension_;
  DataBuffer data_;
};

class TlsExtensionTestBase : public TlsConnectTestBase {
 protected:
  TlsExtensionTestBase(Mode mode, uint16_t version)
    : TlsConnectTestBase(mode, version) {}

  void ClientHelloErrorTest(PacketFilter* filter,
                            uint8_t alert = kTlsAlertDecodeError) {
    auto alert_recorder = new TlsAlertRecorder();
    server_->SetPacketFilter(alert_recorder);
    if (filter) {
      client_->SetPacketFilter(filter);
    }
    ConnectExpectFail();
    EXPECT_EQ(kTlsAlertFatal, alert_recorder->level());
    EXPECT_EQ(alert, alert_recorder->description());
  }

  void ServerHelloErrorTest(PacketFilter* filter,
                            uint8_t alert = kTlsAlertDecodeError) {
    auto alert_recorder = new TlsAlertRecorder();
    client_->SetPacketFilter(alert_recorder);
    if (filter) {
      server_->SetPacketFilter(filter);
    }
    ConnectExpectFail();
    EXPECT_EQ(kTlsAlertFatal, alert_recorder->level());
    EXPECT_EQ(alert, alert_recorder->description());
  }

  static void InitSimpleSni(DataBuffer* extension) {
    const char* name = "host.name";
    const size_t namelen = PL_strlen(name);
    extension->Allocate(namelen + 5);
    extension->Write(0, namelen + 3, 2);
    extension->Write(2, static_cast<uint32_t>(0), 1); // 0 == hostname
    extension->Write(3, namelen, 2);
    extension->Write(5, reinterpret_cast<const uint8_t*>(name), namelen);
  }
};

class TlsExtensionTestDtls
  : public TlsExtensionTestBase,
    public ::testing::WithParamInterface<uint16_t> {
 public:
  TlsExtensionTestDtls() : TlsExtensionTestBase(DGRAM, GetParam()) {}
};

class TlsExtensionTest12Plus
  : public TlsExtensionTestBase,
    public ::testing::WithParamInterface<std::string> {
 public:
  TlsExtensionTest12Plus()
    : TlsExtensionTestBase(TlsConnectTestBase::ToMode(GetParam()),
                           SSL_LIBRARY_VERSION_TLS_1_2) {}
};

class TlsExtensionTest13
  : public TlsExtensionTestBase,
    public ::testing::WithParamInterface<std::string> {
 public:
  TlsExtensionTest13()
    : TlsExtensionTestBase(TlsConnectTestBase::ToMode(GetParam()),
                           SSL_LIBRARY_VERSION_TLS_1_3) {}
};

class TlsExtensionTestGeneric
  : public TlsExtensionTestBase,
    public ::testing::WithParamInterface<std::tuple<std::string, uint16_t>> {
 public:
  TlsExtensionTestGeneric()
    : TlsExtensionTestBase(TlsConnectTestBase::ToMode((std::get<0>(GetParam()))),
                           std::get<1>(GetParam())) {}
};

TEST_P(TlsExtensionTestGeneric, DamageSniLength) {
  ClientHelloErrorTest(new TlsExtensionDamager(ssl_server_name_xtn, 1));
}

TEST_P(TlsExtensionTestGeneric, DamageSniHostLength) {
  ClientHelloErrorTest(new TlsExtensionDamager(ssl_server_name_xtn, 4));
}

TEST_P(TlsExtensionTestGeneric, TruncateSni) {
  ClientHelloErrorTest(new TlsExtensionTruncator(ssl_server_name_xtn, 7));
}

// A valid extension that appears twice will be reported as unsupported.
TEST_P(TlsExtensionTestGeneric, RepeatSni) {
  DataBuffer extension;
  InitSimpleSni(&extension);
  ClientHelloErrorTest(new TlsExtensionInjector(ssl_server_name_xtn, extension),
                       kTlsAlertIllegalParameter);
}

// An SNI entry with zero length is considered invalid (strangely, not if it is
// the last entry, which is probably a bug).
TEST_P(TlsExtensionTestGeneric, BadSni) {
  DataBuffer simple;
  InitSimpleSni(&simple);
  DataBuffer extension;
  extension.Allocate(simple.len() + 3);
  extension.Write(0, static_cast<uint32_t>(0), 3);
  extension.Write(3, simple);
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_server_name_xtn, extension));
}

TEST_P(TlsExtensionTestGeneric, EmptySni) {
  DataBuffer extension;
  extension.Allocate(2);
  extension.Write(0, static_cast<uint32_t>(0), 2);
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_server_name_xtn, extension));
}

TEST_P(TlsExtensionTestGeneric, EmptyAlpnExtension) {
  EnableAlpn();
  DataBuffer extension;
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_app_layer_protocol_xtn, extension),
                       kTlsAlertIllegalParameter);
}

// An empty ALPN isn't considered bad, though it does lead to there being no
// protocol for the server to select.
TEST_P(TlsExtensionTestGeneric, EmptyAlpnList) {
  EnableAlpn();
  const uint8_t val[] = { 0x00, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_app_layer_protocol_xtn, extension),
                       kTlsAlertNoApplicationProtocol);
}

TEST_P(TlsExtensionTestGeneric, OneByteAlpn) {
  EnableAlpn();
  ClientHelloErrorTest(new TlsExtensionTruncator(ssl_app_layer_protocol_xtn, 1));
}

TEST_P(TlsExtensionTestGeneric, AlpnMissingValue) {
  EnableAlpn();
  // This will leave the length of the second entry, but no value.
  ClientHelloErrorTest(new TlsExtensionTruncator(ssl_app_layer_protocol_xtn, 5));
}

TEST_P(TlsExtensionTestGeneric, AlpnZeroLength) {
  EnableAlpn();
  const uint8_t val[] = { 0x01, 0x61, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_app_layer_protocol_xtn, extension));
}

TEST_P(TlsExtensionTestGeneric, AlpnMismatch) {
  const uint8_t client_alpn[] = { 0x01, 0x61 };
  client_->EnableAlpn(client_alpn, sizeof(client_alpn));
  const uint8_t server_alpn[] = { 0x02, 0x61, 0x62 };
  server_->EnableAlpn(server_alpn, sizeof(server_alpn));

  ClientHelloErrorTest(nullptr, kTlsAlertNoApplicationProtocol);
}

TEST_P(TlsExtensionTestGeneric, AlpnReturnedEmptyList) {
  EnableAlpn();
  const uint8_t val[] = { 0x00, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ServerHelloErrorTest(new TlsExtensionReplacer(ssl_app_layer_protocol_xtn, extension));
}

TEST_P(TlsExtensionTestGeneric, AlpnReturnedEmptyName) {
  EnableAlpn();
  const uint8_t val[] = { 0x00, 0x01, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ServerHelloErrorTest(new TlsExtensionReplacer(ssl_app_layer_protocol_xtn, extension));
}

TEST_P(TlsExtensionTestGeneric, AlpnReturnedListTrailingData) {
  EnableAlpn();
  const uint8_t val[] = { 0x00, 0x02, 0x01, 0x61, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ServerHelloErrorTest(new TlsExtensionReplacer(ssl_app_layer_protocol_xtn, extension));
}

TEST_P(TlsExtensionTestGeneric, AlpnReturnedExtraEntry) {
  EnableAlpn();
  const uint8_t val[] = { 0x00, 0x04, 0x01, 0x61, 0x01, 0x62 };
  DataBuffer extension(val, sizeof(val));
  ServerHelloErrorTest(new TlsExtensionReplacer(ssl_app_layer_protocol_xtn, extension));
}

TEST_P(TlsExtensionTestGeneric, AlpnReturnedBadListLength) {
  EnableAlpn();
  const uint8_t val[] = { 0x00, 0x99, 0x01, 0x61, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ServerHelloErrorTest(new TlsExtensionReplacer(ssl_app_layer_protocol_xtn, extension));
}

TEST_P(TlsExtensionTestGeneric, AlpnReturnedBadNameLength) {
  EnableAlpn();
  const uint8_t val[] = { 0x00, 0x02, 0x99, 0x61 };
  DataBuffer extension(val, sizeof(val));
  ServerHelloErrorTest(new TlsExtensionReplacer(ssl_app_layer_protocol_xtn, extension));
}

TEST_P(TlsExtensionTestDtls, SrtpShort) {
  EnableSrtp();
  ClientHelloErrorTest(new TlsExtensionTruncator(ssl_use_srtp_xtn, 3));
}

TEST_P(TlsExtensionTestDtls, SrtpOdd) {
  EnableSrtp();
  const uint8_t val[] = { 0x00, 0x01, 0xff, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_use_srtp_xtn, extension));
}

TEST_P(TlsExtensionTest12Plus, SignatureAlgorithmsBadLength) {
  const uint8_t val[] = { 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_signature_algorithms_xtn,
                                                extension));
}

TEST_P(TlsExtensionTest12Plus, SignatureAlgorithmsTrailingData) {
  const uint8_t val[] = { 0x00, 0x02, 0x04, 0x01, 0x00 }; // sha-256, rsa
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_signature_algorithms_xtn,
                                                extension));
}

TEST_P(TlsExtensionTest12Plus, SignatureAlgorithmsEmpty) {
  const uint8_t val[] = { 0x00, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_signature_algorithms_xtn,
                                                extension));
}

TEST_P(TlsExtensionTest12Plus, SignatureAlgorithmsOddLength) {
  const uint8_t val[] = { 0x00, 0x01, 0x04 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_signature_algorithms_xtn,
                                                extension));
}

// The extension handling ignores unsupported hashes, so breaking that has no
// effect on success rates.  However, ssl3_SendServerKeyExchange catches an
// unsupported signature algorithm.

// This actually fails with a decryption error (fatal alert 51).  That's a bad
// to fail, since any tampering with the handshake will trigger that alert when
// verifying the Finished message.  Thus, this test is disabled until this error
// is turned into an alert.
TEST_P(TlsExtensionTest12Plus, DISABLED_SignatureAlgorithmsSigUnsupported) {
  const uint8_t val[] = { 0x00, 0x02, 0x04, 0x99 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_signature_algorithms_xtn,
                                                extension));
}

TEST_P(TlsExtensionTestGeneric, SupportedCurvesShort) {
  const uint8_t val[] = { 0x00, 0x01, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_elliptic_curves_xtn,
                                                extension));
}

TEST_P(TlsExtensionTestGeneric, SupportedCurvesBadLength) {
  const uint8_t val[] = { 0x09, 0x99, 0x00, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_elliptic_curves_xtn,
                                                extension));
}

TEST_P(TlsExtensionTestGeneric, SupportedCurvesTrailingData) {
  const uint8_t val[] = { 0x00, 0x02, 0x00, 0x00, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_elliptic_curves_xtn,
                                                extension));
}

TEST_P(TlsExtensionTestGeneric, SupportedPointsEmpty) {
  const uint8_t val[] = { 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_ec_point_formats_xtn,
                                                extension));
}

TEST_P(TlsExtensionTestGeneric, SupportedPointsBadLength) {
  const uint8_t val[] = { 0x99, 0x00, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_ec_point_formats_xtn,
                                                extension));
}

TEST_P(TlsExtensionTestGeneric, SupportedPointsTrailingData) {
  const uint8_t val[] = { 0x01, 0x00, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_ec_point_formats_xtn,
                                                extension));
}

TEST_P(TlsExtensionTestGeneric, RenegotiationInfoBadLength) {
  const uint8_t val[] = { 0x99 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_renegotiation_info_xtn,
                                                extension));
}

TEST_P(TlsExtensionTestGeneric, RenegotiationInfoMismatch) {
  const uint8_t val[] = { 0x01, 0x00 };
  DataBuffer extension(val, sizeof(val));
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_renegotiation_info_xtn,
                                                extension));
}

// The extension has to contain a length.
TEST_P(TlsExtensionTestGeneric, RenegotiationInfoExtensionEmpty) {
  DataBuffer extension;
  ClientHelloErrorTest(new TlsExtensionReplacer(ssl_renegotiation_info_xtn,
                                                extension));
}

TEST_P(TlsExtensionTest12Plus, SignatureAlgorithmConfiguration) {
  const SSLSignatureAndHashAlg algorithms[] = {
    {ssl_hash_sha512, ssl_sign_rsa},
    {ssl_hash_sha384, ssl_sign_ecdsa}
  };

  TlsExtensionCapture *capture =
    new TlsExtensionCapture(ssl_signature_algorithms_xtn);
  client_->SetSignatureAlgorithms(algorithms, PR_ARRAY_SIZE(algorithms));
  client_->SetPacketFilter(capture);
  DisableDheAndEcdheCiphers();
  Connect();

  const DataBuffer& ext = capture->extension();
  EXPECT_EQ(2 + PR_ARRAY_SIZE(algorithms) * 2, ext.len());
  for (size_t i = 0, cursor = 2;
       i < PR_ARRAY_SIZE(algorithms) && cursor < ext.len();
       ++i) {
    uint32_t v;
    EXPECT_TRUE(ext.Read(cursor++, 1, &v));
    EXPECT_EQ(algorithms[i].hashAlg, static_cast<SSLHashType>(v));
    EXPECT_TRUE(ext.Read(cursor++, 1, &v));
    EXPECT_EQ(algorithms[i].sigAlg, static_cast<SSLSignType>(v));
  }
}

/*
 * Tests for Certificate Transparency (RFC 6962)
 */

// Helper class - stores signed certificate timestamps as provided
// by the relevant callbacks on the client.
class SignedCertificateTimestampsExtractor {
 public:
  SignedCertificateTimestampsExtractor(TlsAgent& client) {
    client.SetAuthCertificateCallback(
      [&](TlsAgent& agent, PRBool checksig, PRBool isServer) -> SECStatus {
        const SECItem *scts = SSL_PeerSignedCertTimestamps(agent.ssl_fd());
        EXPECT_TRUE(scts);
        if (!scts) {
          return SECFailure;
        }
        auth_timestamps_.reset(new DataBuffer(scts->data, scts->len));
        return SECSuccess;
      }
    );
    client.SetHandshakeCallback(
      [&](TlsAgent& agent) {
        const SECItem *scts = SSL_PeerSignedCertTimestamps(agent.ssl_fd());
        ASSERT_TRUE(scts);
        handshake_timestamps_.reset(new DataBuffer(scts->data, scts->len));
      }
    );
  }

  void assertTimestamps(const DataBuffer& timestamps) {
    ASSERT_TRUE(auth_timestamps_);
    ASSERT_EQ(timestamps, *auth_timestamps_);

    ASSERT_TRUE(handshake_timestamps_);
    ASSERT_EQ(timestamps, *handshake_timestamps_);
  }

 private:
  std::unique_ptr<DataBuffer> auth_timestamps_;
  std::unique_ptr<DataBuffer> handshake_timestamps_;
};

// Test timestamps extraction during a successful handshake.
TEST_P(TlsExtensionTestGeneric, SignedCertificateTimestampsHandshake) {
  uint8_t val[] = { 0x01, 0x23, 0x45, 0x67, 0x89 };
  const SECItem si_timestamps = { siBuffer, val, sizeof(val) };
  const DataBuffer timestamps(val, sizeof(val));

  server_->StartConnect();
  ASSERT_EQ(SECSuccess,
    SSL_SetSignedCertTimestamps(server_->ssl_fd(),
      &si_timestamps, server_->kea()));

  client_->StartConnect();
  ASSERT_EQ(SECSuccess,
    SSL_OptionSet(client_->ssl_fd(),
      SSL_ENABLE_SIGNED_CERT_TIMESTAMPS, PR_TRUE));

  SignedCertificateTimestampsExtractor timestamps_extractor(*client_);
  Handshake();
  CheckConnected();
  timestamps_extractor.assertTimestamps(timestamps);
}

// Test SSL_PeerSignedCertTimestamps returning zero-length SECItem
// when the client / the server / both have not enabled the feature.
TEST_P(TlsExtensionTestGeneric, SignedCertificateTimestampsInactiveClient) {
  uint8_t val[] = { 0x01, 0x23, 0x45, 0x67, 0x89 };
  const SECItem si_timestamps = { siBuffer, val, sizeof(val) };

  server_->StartConnect();
  ASSERT_EQ(SECSuccess,
    SSL_SetSignedCertTimestamps(server_->ssl_fd(),
      &si_timestamps, server_->kea()));

  client_->StartConnect();

  SignedCertificateTimestampsExtractor timestamps_extractor(*client_);
  Handshake();
  CheckConnected();
  timestamps_extractor.assertTimestamps(DataBuffer());
}

TEST_P(TlsExtensionTestGeneric, SignedCertificateTimestampsInactiveServer) {
  server_->StartConnect();

  client_->StartConnect();
  ASSERT_EQ(SECSuccess,
    SSL_OptionSet(client_->ssl_fd(),
      SSL_ENABLE_SIGNED_CERT_TIMESTAMPS, PR_TRUE));

  SignedCertificateTimestampsExtractor timestamps_extractor(*client_);
  Handshake();
  CheckConnected();
  timestamps_extractor.assertTimestamps(DataBuffer());
}

TEST_P(TlsExtensionTestGeneric, SignedCertificateTimestampsInactiveBoth) {
  server_->StartConnect();
  client_->StartConnect();

  SignedCertificateTimestampsExtractor timestamps_extractor(*client_);
  Handshake();
  CheckConnected();
  timestamps_extractor.assertTimestamps(DataBuffer());
}


// Temporary test to verify that we choke on an empty ClientKeyShare.
// This test will fail when we implement HelloRetryRequest.
TEST_P(TlsExtensionTest13, EmptyClientKeyShare) {
  ClientHelloErrorTest(new TlsExtensionTruncator(ssl_tls13_key_share_xtn, 2),
                       kTlsAlertHandshakeFailure);
}

INSTANTIATE_TEST_CASE_P(ExtensionTls10, TlsExtensionTestGeneric,
                        ::testing::Combine(
                          TlsConnectTestBase::kTlsModesStream,
                          TlsConnectTestBase::kTlsV10));
INSTANTIATE_TEST_CASE_P(ExtensionVariants, TlsExtensionTestGeneric,
                        ::testing::Combine(
                          TlsConnectTestBase::kTlsModesAll,
                          TlsConnectTestBase::kTlsV11V12));
INSTANTIATE_TEST_CASE_P(ExtensionTls12Plus, TlsExtensionTest12Plus,
                        TlsConnectTestBase::kTlsModesAll);
#ifdef NSS_ENABLE_TLS_1_3
INSTANTIATE_TEST_CASE_P(ExtensionTls13, TlsExtensionTest13,
                        TlsConnectTestBase::kTlsModesStream);
#endif
INSTANTIATE_TEST_CASE_P(ExtensionDgram, TlsExtensionTestDtls,
                        TlsConnectTestBase::kTlsV11V12);

}  // namespace nspr_test
