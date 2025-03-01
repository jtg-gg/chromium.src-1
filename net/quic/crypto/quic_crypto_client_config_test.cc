// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/crypto/quic_crypto_client_config.h"

#include "net/quic/crypto/proof_verifier.h"
#include "net/quic/quic_server_id.h"
#include "net/quic/test_tools/crypto_test_utils.h"
#include "net/quic/test_tools/mock_random.h"
#include "net/quic/test_tools/quic_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"

using std::string;
using std::vector;

namespace net {
namespace test {
namespace {

class TestProofVerifyDetails : public ProofVerifyDetails {
  ~TestProofVerifyDetails() override {}

  // ProofVerifyDetails implementation
  ProofVerifyDetails* Clone() const override {
    return new TestProofVerifyDetails;
  }
};

}  // namespace

TEST(QuicCryptoClientConfigTest, CachedState_IsEmpty) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_TRUE(state.IsEmpty());
}

TEST(QuicCryptoClientConfigTest, CachedState_IsComplete) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_FALSE(state.IsComplete(QuicWallTime::FromUNIXSeconds(0)));
}

TEST(QuicCryptoClientConfigTest, CachedState_GenerationCounter) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_EQ(0u, state.generation_counter());
  state.SetProofInvalid();
  EXPECT_EQ(1u, state.generation_counter());
}

TEST(QuicCryptoClientConfigTest, CachedState_SetProofVerifyDetails) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_TRUE(state.proof_verify_details() == nullptr);
  ProofVerifyDetails* details = new TestProofVerifyDetails;
  state.SetProofVerifyDetails(details);
  EXPECT_EQ(details, state.proof_verify_details());
}

TEST(QuicCryptoClientConfigTest, CachedState_ServerDesignatedConnectionId) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_FALSE(state.has_server_designated_connection_id());

  QuicConnectionId connection_id = 1234;
  state.add_server_designated_connection_id(connection_id);
  EXPECT_TRUE(state.has_server_designated_connection_id());
  EXPECT_EQ(connection_id, state.GetNextServerDesignatedConnectionId());
  EXPECT_FALSE(state.has_server_designated_connection_id());

  // Allow the ID to be set multiple times.  It's unusual that this would
  // happen, but not impossible.
  ++connection_id;
  state.add_server_designated_connection_id(connection_id);
  EXPECT_TRUE(state.has_server_designated_connection_id());
  EXPECT_EQ(connection_id, state.GetNextServerDesignatedConnectionId());
  ++connection_id;
  state.add_server_designated_connection_id(connection_id);
  EXPECT_EQ(connection_id, state.GetNextServerDesignatedConnectionId());
  EXPECT_FALSE(state.has_server_designated_connection_id());

  // Test FIFO behavior.
  const QuicConnectionId first_cid = 0xdeadbeef;
  const QuicConnectionId second_cid = 0xfeedbead;
  state.add_server_designated_connection_id(first_cid);
  state.add_server_designated_connection_id(second_cid);
  EXPECT_TRUE(state.has_server_designated_connection_id());
  EXPECT_EQ(first_cid, state.GetNextServerDesignatedConnectionId());
  EXPECT_EQ(second_cid, state.GetNextServerDesignatedConnectionId());
}

TEST(QuicCryptoClientConfigTest, CachedState_ServerIdConsumedBeforeSet) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_FALSE(state.has_server_designated_connection_id());
#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
  EXPECT_DEBUG_DEATH(state.GetNextServerDesignatedConnectionId(),
                     "Attempting to consume a connection id "
                     "that was never designated.");
#endif  // GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
}

TEST(QuicCryptoClientConfigTest, CachedState_ServerNonce) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_FALSE(state.has_server_nonce());

  string server_nonce = "nonce_1";
  state.add_server_nonce(server_nonce);
  EXPECT_TRUE(state.has_server_nonce());
  EXPECT_EQ(server_nonce, state.GetNextServerNonce());
  EXPECT_FALSE(state.has_server_nonce());

  // Allow the ID to be set multiple times.  It's unusual that this would
  // happen, but not impossible.
  server_nonce = "nonce_2";
  state.add_server_nonce(server_nonce);
  EXPECT_TRUE(state.has_server_nonce());
  EXPECT_EQ(server_nonce, state.GetNextServerNonce());
  server_nonce = "nonce_3";
  state.add_server_nonce(server_nonce);
  EXPECT_EQ(server_nonce, state.GetNextServerNonce());
  EXPECT_FALSE(state.has_server_nonce());

  // Test FIFO behavior.
  const string first_nonce = "first_nonce";
  const string second_nonce = "second_nonce";
  state.add_server_nonce(first_nonce);
  state.add_server_nonce(second_nonce);
  EXPECT_TRUE(state.has_server_nonce());
  EXPECT_EQ(first_nonce, state.GetNextServerNonce());
  EXPECT_EQ(second_nonce, state.GetNextServerNonce());
}

TEST(QuicCryptoClientConfigTest, CachedState_ServerNonceConsumedBeforeSet) {
  QuicCryptoClientConfig::CachedState state;
  EXPECT_FALSE(state.has_server_nonce());
#if GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
  EXPECT_DEBUG_DEATH(state.GetNextServerNonce(),
                     "Attempting to consume a server nonce "
                     "that was never designated.");
#endif  // GTEST_HAS_DEATH_TEST && !defined(NDEBUG)
}

TEST(QuicCryptoClientConfigTest, CachedState_InitializeFrom) {
  QuicCryptoClientConfig::CachedState state;
  QuicCryptoClientConfig::CachedState other;
  state.set_source_address_token("TOKEN");
  // TODO(rch): Populate other fields of |state|.
  other.InitializeFrom(state);
  EXPECT_EQ(state.server_config(), other.server_config());
  EXPECT_EQ(state.source_address_token(), other.source_address_token());
  EXPECT_EQ(state.certs(), other.certs());
  EXPECT_EQ(1u, other.generation_counter());
  EXPECT_FALSE(state.has_server_designated_connection_id());
  EXPECT_FALSE(state.has_server_nonce());
}

TEST(QuicCryptoClientConfigTest, InchoateChlo) {
  QuicCryptoClientConfig::CachedState state;
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  QuicCryptoNegotiatedParameters params;
  CryptoHandshakeMessage msg;
  QuicServerId server_id("www.google.com", 80, PRIVACY_MODE_DISABLED);
  MockRandom rand;
  config.FillInchoateClientHello(server_id, QuicVersionMax(), &state, &rand,
                                 &params, &msg);

  QuicTag cver;
  EXPECT_EQ(QUIC_NO_ERROR, msg.GetUint32(kVER, &cver));
  EXPECT_EQ(QuicVersionToQuicTag(QuicVersionMax()), cver);
  StringPiece proof_nonce;
  EXPECT_TRUE(msg.GetStringPiece(kNONP, &proof_nonce));
  EXPECT_EQ(string(32, 'r'), proof_nonce);
}

TEST(QuicCryptoClientConfigTest, PreferAesGcm) {
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  if (config.aead.size() > 1)
    EXPECT_NE(kAESG, config.aead[0]);
  config.PreferAesGcm();
  EXPECT_EQ(kAESG, config.aead[0]);
}

TEST(QuicCryptoClientConfigTest, InchoateChloSecure) {
  QuicCryptoClientConfig::CachedState state;
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  QuicCryptoNegotiatedParameters params;
  CryptoHandshakeMessage msg;
  QuicServerId server_id("www.google.com", 443, PRIVACY_MODE_DISABLED);
  MockRandom rand;
  config.FillInchoateClientHello(server_id, QuicVersionMax(), &state, &rand,
                                 &params, &msg);

  QuicTag pdmd;
  EXPECT_EQ(QUIC_NO_ERROR, msg.GetUint32(kPDMD, &pdmd));
  EXPECT_EQ(kX509, pdmd);
  StringPiece scid;
  EXPECT_FALSE(msg.GetStringPiece(kSCID, &scid));
}

TEST(QuicCryptoClientConfigTest, InchoateChloSecureWithSCID) {
  QuicCryptoClientConfig::CachedState state;
  CryptoHandshakeMessage scfg;
  scfg.set_tag(kSCFG);
  uint64_t future = 1;
  scfg.SetValue(kEXPY, future);
  scfg.SetStringPiece(kSCID, "12345678");
  string details;
  state.SetServerConfig(scfg.GetSerialized().AsStringPiece(),
                        QuicWallTime::FromUNIXSeconds(0), &details);

  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  QuicCryptoNegotiatedParameters params;
  CryptoHandshakeMessage msg;
  QuicServerId server_id("www.google.com", 443, PRIVACY_MODE_DISABLED);
  MockRandom rand;
  config.FillInchoateClientHello(server_id, QuicVersionMax(), &state, &rand,
                                 &params, &msg);

  StringPiece scid;
  EXPECT_TRUE(msg.GetStringPiece(kSCID, &scid));
  EXPECT_EQ("12345678", scid);
}

TEST(QuicCryptoClientConfigTest, InchoateChloSecureNoEcdsa) {
  QuicCryptoClientConfig::CachedState state;
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  config.DisableEcdsa();
  QuicCryptoNegotiatedParameters params;
  CryptoHandshakeMessage msg;
  QuicServerId server_id("www.google.com", 443, PRIVACY_MODE_DISABLED);
  MockRandom rand;
  config.FillInchoateClientHello(server_id, QuicVersionMax(), &state, &rand,
                                 &params, &msg);

  QuicTag pdmd;
  EXPECT_EQ(QUIC_NO_ERROR, msg.GetUint32(kPDMD, &pdmd));
  EXPECT_EQ(kX59R, pdmd);
}

TEST(QuicCryptoClientConfigTest, FillClientHello) {
  QuicCryptoClientConfig::CachedState state;
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  QuicCryptoNegotiatedParameters params;
  QuicConnectionId kConnectionId = 1234;
  string error_details;
  MockRandom rand;
  CryptoHandshakeMessage chlo;
  QuicServerId server_id("www.google.com", 80, PRIVACY_MODE_DISABLED);
  config.FillClientHello(server_id, kConnectionId, QuicVersionMax(), &state,
                         QuicWallTime::Zero(), &rand,
                         nullptr,  // channel_id_key
                         &params, &chlo, &error_details);

  // Verify that certain QuicTags have been set correctly in the CHLO.
  QuicTag cver;
  EXPECT_EQ(QUIC_NO_ERROR, chlo.GetUint32(kVER, &cver));
  EXPECT_EQ(QuicVersionToQuicTag(QuicVersionMax()), cver);
}

TEST(QuicCryptoClientConfigTest, ProcessServerDowngradeAttack) {
  QuicVersionVector supported_versions = QuicSupportedVersions();
  if (supported_versions.size() == 1) {
    // No downgrade attack is possible if the client only supports one version.
    return;
  }
  QuicTagVector supported_version_tags;
  for (size_t i = supported_versions.size(); i > 0; --i) {
    supported_version_tags.push_back(
        QuicVersionToQuicTag(supported_versions[i - 1]));
  }
  CryptoHandshakeMessage msg;
  msg.set_tag(kSHLO);
  msg.SetVector(kVER, supported_version_tags);

  QuicCryptoClientConfig::CachedState cached;
  QuicCryptoNegotiatedParameters out_params;
  string error;
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  EXPECT_EQ(QUIC_VERSION_NEGOTIATION_MISMATCH,
            config.ProcessServerHello(msg, 0, supported_versions.front(),
                                      supported_versions, &cached, &out_params,
                                      &error));
  EXPECT_EQ("Downgrade attack detected", error);
}

TEST(QuicCryptoClientConfigTest, InitializeFrom) {
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  QuicServerId canonical_server_id("www.google.com", 80, PRIVACY_MODE_DISABLED);
  QuicCryptoClientConfig::CachedState* state =
      config.LookupOrCreate(canonical_server_id);
  // TODO(rch): Populate other fields of |state|.
  state->set_source_address_token("TOKEN");
  state->SetProofValid();

  QuicServerId other_server_id("mail.google.com", 80, PRIVACY_MODE_DISABLED);
  config.InitializeFrom(other_server_id, canonical_server_id, &config);
  QuicCryptoClientConfig::CachedState* other =
      config.LookupOrCreate(other_server_id);

  EXPECT_EQ(state->server_config(), other->server_config());
  EXPECT_EQ(state->source_address_token(), other->source_address_token());
  EXPECT_EQ(state->certs(), other->certs());
  EXPECT_EQ(1u, other->generation_counter());
}

TEST(QuicCryptoClientConfigTest, Canonical) {
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  config.AddCanonicalSuffix(".google.com");
  QuicServerId canonical_id1("www.google.com", 80, PRIVACY_MODE_DISABLED);
  QuicServerId canonical_id2("mail.google.com", 80, PRIVACY_MODE_DISABLED);
  QuicCryptoClientConfig::CachedState* state =
      config.LookupOrCreate(canonical_id1);
  // TODO(rch): Populate other fields of |state|.
  state->set_source_address_token("TOKEN");
  state->SetProofValid();

  QuicCryptoClientConfig::CachedState* other =
      config.LookupOrCreate(canonical_id2);

  EXPECT_TRUE(state->IsEmpty());
  EXPECT_EQ(state->server_config(), other->server_config());
  EXPECT_EQ(state->source_address_token(), other->source_address_token());
  EXPECT_EQ(state->certs(), other->certs());
  EXPECT_EQ(1u, other->generation_counter());

  QuicServerId different_id("mail.google.org", 80, PRIVACY_MODE_DISABLED);
  EXPECT_TRUE(config.LookupOrCreate(different_id)->IsEmpty());
}

TEST(QuicCryptoClientConfigTest, CanonicalNotUsedIfNotValid) {
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  config.AddCanonicalSuffix(".google.com");
  QuicServerId canonical_id1("www.google.com", 80, PRIVACY_MODE_DISABLED);
  QuicServerId canonical_id2("mail.google.com", 80, PRIVACY_MODE_DISABLED);
  QuicCryptoClientConfig::CachedState* state =
      config.LookupOrCreate(canonical_id1);
  // TODO(rch): Populate other fields of |state|.
  state->set_source_address_token("TOKEN");

  // Do not set the proof as valid, and check that it is not used
  // as a canonical entry.
  EXPECT_TRUE(config.LookupOrCreate(canonical_id2)->IsEmpty());
}

TEST(QuicCryptoClientConfigTest, ClearCachedStates) {
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  QuicServerId server_id("www.google.com", 80, PRIVACY_MODE_DISABLED);
  QuicCryptoClientConfig::CachedState* state = config.LookupOrCreate(server_id);
  // TODO(rch): Populate other fields of |state|.
  vector<string> certs(1);
  certs[0] = "Hello Cert";
  state->SetProof(certs, "cert_sct", "signature");
  state->set_source_address_token("TOKEN");
  state->SetProofValid();
  EXPECT_EQ(1u, state->generation_counter());

  // Verify LookupOrCreate returns the same data.
  QuicCryptoClientConfig::CachedState* other = config.LookupOrCreate(server_id);

  EXPECT_EQ(state, other);
  EXPECT_EQ(1u, other->generation_counter());

  // Clear the cached states.
  config.ClearCachedStates();

  // Verify LookupOrCreate doesn't have any data.
  QuicCryptoClientConfig::CachedState* cleared_cache =
      config.LookupOrCreate(server_id);

  EXPECT_EQ(state, cleared_cache);
  EXPECT_FALSE(cleared_cache->proof_valid());
  EXPECT_TRUE(cleared_cache->server_config().empty());
  EXPECT_TRUE(cleared_cache->certs().empty());
  EXPECT_TRUE(cleared_cache->cert_sct().empty());
  EXPECT_TRUE(cleared_cache->signature().empty());
  EXPECT_EQ(2u, cleared_cache->generation_counter());
}

TEST(QuicCryptoClientConfigTest, ProcessReject) {
  CryptoHandshakeMessage rej;
  CryptoTestUtils::FillInDummyReject(&rej, /* stateless */ false);

  // Now process the rejection.
  QuicCryptoClientConfig::CachedState cached;
  QuicCryptoNegotiatedParameters out_params;
  string error;
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  EXPECT_EQ(QUIC_NO_ERROR,
            config.ProcessRejection(rej, QuicWallTime::FromUNIXSeconds(0),
                                    QuicSupportedVersions().front(), &cached,
                                    &out_params, &error));
  EXPECT_FALSE(cached.has_server_designated_connection_id());
  EXPECT_FALSE(cached.has_server_nonce());
}

TEST(QuicCryptoClientConfigTest, ProcessStatelessReject) {
  // Create a dummy reject message and mark it as stateless.
  CryptoHandshakeMessage rej;
  CryptoTestUtils::FillInDummyReject(&rej, /* stateless */ true);
  const QuicConnectionId kConnectionId = 0xdeadbeef;
  const string server_nonce = "SERVER_NONCE";
  rej.SetValue(kRCID, kConnectionId);
  rej.SetStringPiece(kServerNonceTag, server_nonce);

  // Now process the rejection.
  QuicCryptoClientConfig::CachedState cached;
  QuicCryptoNegotiatedParameters out_params;
  string error;
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  EXPECT_EQ(QUIC_NO_ERROR,
            config.ProcessRejection(rej, QuicWallTime::FromUNIXSeconds(0),
                                    QuicSupportedVersions().front(), &cached,
                                    &out_params, &error));
  EXPECT_TRUE(cached.has_server_designated_connection_id());
  EXPECT_EQ(kConnectionId, cached.GetNextServerDesignatedConnectionId());
  EXPECT_EQ(server_nonce, cached.GetNextServerNonce());
}

TEST(QuicCryptoClientConfigTest, BadlyFormattedStatelessReject) {
  // Create a dummy reject message and mark it as stateless.  Do not
  // add an server-designated connection-id.
  CryptoHandshakeMessage rej;
  CryptoTestUtils::FillInDummyReject(&rej, /* stateless */ true);

  // Now process the rejection.
  QuicCryptoClientConfig::CachedState cached;
  QuicCryptoNegotiatedParameters out_params;
  string error;
  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  EXPECT_EQ(QUIC_CRYPTO_MESSAGE_PARAMETER_NOT_FOUND,
            config.ProcessRejection(rej, QuicWallTime::FromUNIXSeconds(0),
                                    QuicSupportedVersions().front(), &cached,
                                    &out_params, &error));
  EXPECT_FALSE(cached.has_server_designated_connection_id());
  EXPECT_EQ("Missing kRCID", error);
}

TEST(QuicCryptoClientConfigTest, ServerNonceinSHLO_BeforeQ027) {
  // Test that in QUIC_VERSION_26 and lower, the the server does not need to
  // include a nonce in the SHLO.
  CryptoHandshakeMessage msg;
  msg.set_tag(kSHLO);
  // Choose the lowest version.
  QuicVersionVector supported_versions;
  QuicVersion version = QuicSupportedVersions().back();
  supported_versions.push_back(version);
  EXPECT_LE(version, QUIC_VERSION_26);
  QuicTagVector versions;
  versions.push_back(QuicVersionToQuicTag(version));
  msg.SetVector(kVER, versions);

  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  QuicCryptoClientConfig::CachedState cached;
  QuicCryptoNegotiatedParameters out_params;
  string error_details;
  config.ProcessServerHello(msg, 0, version, supported_versions, &cached,
                            &out_params, &error_details);
  EXPECT_NE("server hello missing server nonce", error_details);
}

TEST(QuicCryptoClientConfigTest, ServerNonceinSHLO_AfterQ027) {
  // Test that in QUIC_VERSION_27 and higher, the the server must include a
  // nonce in the SHLO.
  CryptoHandshakeMessage msg;
  msg.set_tag(kSHLO);
  // Choose the latest version.
  QuicVersionVector supported_versions;
  QuicVersion version = QuicSupportedVersions().front();
  supported_versions.push_back(version);
  EXPECT_LE(QUIC_VERSION_27, version);
  QuicTagVector versions;
  versions.push_back(QuicVersionToQuicTag(version));
  msg.SetVector(kVER, versions);

  QuicCryptoClientConfig config(CryptoTestUtils::ProofVerifierForTesting());
  QuicCryptoClientConfig::CachedState cached;
  QuicCryptoNegotiatedParameters out_params;
  string error_details;
  EXPECT_EQ(QUIC_INVALID_CRYPTO_MESSAGE_PARAMETER,
            config.ProcessServerHello(msg, 0, version, supported_versions,
                                      &cached, &out_params, &error_details));
  EXPECT_EQ("server hello missing server nonce", error_details);
}

}  // namespace test
}  // namespace net
