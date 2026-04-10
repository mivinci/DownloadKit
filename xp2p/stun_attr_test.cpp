/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * stun_attr_test.cpp - Unit tests for STUN attribute encoding / decoding
 */

#include <gtest/gtest.h>

extern "C" {
#include "stun_attr.h"
#include "stun_msg.h"
}

#include <arpa/inet.h>

class StunAttrTest : public ::testing::Test {
protected:
  uint8_t msg_buf[512];
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];
  xStunAttrWriter writer;

  void SetUp() override {
    memset(msg_buf, 0, sizeof(msg_buf));
    for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++) {
      txn_id[i] = (uint8_t)(0xB0 + i);
    }
    /* Write a STUN header first */
    xStunMsg msg;
    xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);
    xStunMsgEncode(&msg, msg_buf, sizeof(msg_buf));

    /* Init writer at attribute area */
    xStunAttrWriterInit(&writer, msg_buf + XSTUN_HEADER_SIZE,
                        sizeof(msg_buf) - XSTUN_HEADER_SIZE);
  }

  /* Helper: build a decodable message from current writer state */
  void FinalizeMsg(size_t *total_len) {
    xWriteU16BE(msg_buf + 2, (uint16_t)writer.pos);
    *total_len = XSTUN_HEADER_SIZE + writer.pos;
  }
};

/* ───────────────────── XOR-MAPPED-ADDRESS ───────────────────── */

TEST_F(StunAttrTest, XorMappedAddressIPv4RoundTrip) {
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(12345);
  inet_pton(AF_INET, "192.168.1.100", &addr.sin_addr);

  xErrno err = xStunAttrWriteXorMappedAddress(
      &writer, (struct sockaddr *)&addr, txn_id);
  ASSERT_EQ(err, xErrno_Ok);

  /* Decode */
  size_t total;
  FinalizeMsg(&total);

  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_XorMappedAddress);

  struct sockaddr_storage out;
  ASSERT_EQ(xStunAttrDecodeXorMappedAddress(&attr, txn_id, &out),
            xErrno_Ok);

  struct sockaddr_in *out4 = (struct sockaddr_in *)&out;
  EXPECT_EQ(out4->sin_family, AF_INET);
  EXPECT_EQ(ntohs(out4->sin_port), 12345);
  EXPECT_EQ(out4->sin_addr.s_addr, addr.sin_addr.s_addr);
}

TEST_F(StunAttrTest, XorMappedAddressIPv6RoundTrip) {
  struct sockaddr_in6 addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(54321);
  inet_pton(AF_INET6, "2001:db8::1", &addr.sin6_addr);

  xErrno err = xStunAttrWriteXorMappedAddress(
      &writer, (struct sockaddr *)&addr, txn_id);
  ASSERT_EQ(err, xErrno_Ok);

  size_t total;
  FinalizeMsg(&total);

  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));

  struct sockaddr_storage out;
  ASSERT_EQ(xStunAttrDecodeXorMappedAddress(&attr, txn_id, &out),
            xErrno_Ok);

  struct sockaddr_in6 *out6 = (struct sockaddr_in6 *)&out;
  EXPECT_EQ(out6->sin6_family, AF_INET6);
  EXPECT_EQ(ntohs(out6->sin6_port), 54321);
  EXPECT_EQ(memcmp(&out6->sin6_addr, &addr.sin6_addr, 16), 0);
}

/* ───────────────────── USERNAME ───────────────────── */

TEST_F(StunAttrTest, UsernameRoundTrip) {
  xErrno err = xStunAttrWriteUsername(&writer, "remote_ufrag", "local_ufrag");
  ASSERT_EQ(err, xErrno_Ok);

  size_t total;
  FinalizeMsg(&total);

  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_Username);

  std::string username((const char *)attr.value, attr.length);
  EXPECT_EQ(username, "remote_ufrag:local_ufrag");
}

/* ───────────────────── PRIORITY ───────────────────── */

TEST_F(StunAttrTest, PriorityRoundTrip) {
  uint32_t prio = 0x6E001FFF;
  ASSERT_EQ(xStunAttrWritePriority(&writer, prio), xErrno_Ok);

  size_t total;
  FinalizeMsg(&total);

  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_Priority);
  EXPECT_EQ(attr.length, 4);
  EXPECT_EQ(xReadU32BE(attr.value), prio);
}

/* ───────────────────── USE-CANDIDATE ───────────────────── */

TEST_F(StunAttrTest, UseCandidateZeroLength) {
  ASSERT_EQ(xStunAttrWriteUseCandidate(&writer), xErrno_Ok);

  size_t total;
  FinalizeMsg(&total);

  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_UseCandidate);
  EXPECT_EQ(attr.length, 0);
}

/* ───────────────────── ICE-CONTROLLING / ICE-CONTROLLED ───────────────────── */

TEST_F(StunAttrTest, IceControllingRoundTrip) {
  uint64_t tie = 0x123456789ABCDEF0ULL;
  ASSERT_EQ(xStunAttrWriteIceControlling(&writer, tie), xErrno_Ok);

  size_t total;
  FinalizeMsg(&total);

  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_IceControlling);
  EXPECT_EQ(attr.length, 8);
  EXPECT_EQ(xReadU64BE(attr.value), tie);
}

/* ───────────────────── ERROR-CODE ───────────────────── */

TEST_F(StunAttrTest, ErrorCodeRoundTrip) {
  ASSERT_EQ(xStunAttrWriteErrorCode(&writer, 401, "Unauthorized"),
            xErrno_Ok);

  size_t total;
  FinalizeMsg(&total);

  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_ErrorCode);

  int code;
  const char *reason;
  size_t reason_len;
  ASSERT_EQ(xStunAttrDecodeErrorCode(&attr, &code, &reason, &reason_len),
            xErrno_Ok);
  EXPECT_EQ(code, 401);
  EXPECT_EQ(std::string(reason, reason_len), "Unauthorized");
}

/* ───────────────────── MESSAGE-INTEGRITY ───────────────────── */

TEST_F(StunAttrTest, MessageIntegrityWriteAndVerify) {
  const char *key_str = "test_password";
  const uint8_t *key = (const uint8_t *)key_str;
  size_t key_len = strlen(key_str);

  /* Write some attributes first */
  ASSERT_EQ(xStunAttrWritePriority(&writer, 0x12345678), xErrno_Ok);
  ASSERT_EQ(xStunAttrWriteMessageIntegrity(&writer, msg_buf, key, key_len),
            xErrno_Ok);

  size_t total;
  FinalizeMsg(&total);

  /* Decode and verify */
  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;

  /* Skip PRIORITY */
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_Priority);

  /* MESSAGE-INTEGRITY */
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_MessageIntegrity);
  EXPECT_EQ(attr.length, XSTUN_SHA1_DIGEST_SIZE);

  EXPECT_EQ(xStunAttrVerifyMessageIntegrity(msg_buf, total, &attr,
                                             key, key_len),
            xErrno_Ok);

  /* Verify with wrong key should fail */
  const char *wrong_key = "wrong_password";
  EXPECT_NE(xStunAttrVerifyMessageIntegrity(msg_buf, total, &attr,
                                             (const uint8_t *)wrong_key,
                                             strlen(wrong_key)),
            xErrno_Ok);
}

/* ───────────────────── FINGERPRINT ───────────────────── */

TEST_F(StunAttrTest, FingerprintWriteAndVerify) {
  ASSERT_EQ(xStunAttrWritePriority(&writer, 0xAABBCCDD), xErrno_Ok);
  ASSERT_EQ(xStunAttrWriteFingerprint(&writer, msg_buf), xErrno_Ok);

  size_t total;
  FinalizeMsg(&total);

  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;

  /* Skip PRIORITY */
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));

  /* FINGERPRINT */
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_Fingerprint);
  EXPECT_EQ(attr.length, 4);

  EXPECT_EQ(xStunAttrVerifyFingerprint(msg_buf, total, &attr), xErrno_Ok);

  /* Corrupt a byte and verify should fail */
  msg_buf[XSTUN_HEADER_SIZE + 5] ^= 0xFF;
  EXPECT_NE(xStunAttrVerifyFingerprint(msg_buf, total, &attr), xErrno_Ok);
}

/* ───────────────────── Multiple Attributes ───────────────────── */

TEST_F(StunAttrTest, MultipleAttributeIteration) {
  ASSERT_EQ(xStunAttrWritePriority(&writer, 100), xErrno_Ok);
  ASSERT_EQ(xStunAttrWriteUseCandidate(&writer), xErrno_Ok);
  ASSERT_EQ(xStunAttrWriteIceControlling(&writer, 42), xErrno_Ok);

  size_t total;
  FinalizeMsg(&total);

  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;

  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_Priority);

  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_UseCandidate);

  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_IceControlling);

  EXPECT_FALSE(xStunAttrIterNext(&iter, &attr));
}

/* ───────────────────── Comprehension Required/Optional ───────────────────── */

TEST_F(StunAttrTest, ComprehensionRequired) {
  EXPECT_TRUE(xStunAttrIsComprehensionRequired(xStunAttrType_MappedAddress));
  EXPECT_TRUE(xStunAttrIsComprehensionRequired(xStunAttrType_Username));
  EXPECT_TRUE(xStunAttrIsComprehensionRequired(xStunAttrType_Priority));
  EXPECT_FALSE(xStunAttrIsComprehensionRequired(xStunAttrType_Software));
  EXPECT_FALSE(xStunAttrIsComprehensionRequired(xStunAttrType_Fingerprint));
  EXPECT_FALSE(xStunAttrIsComprehensionRequired(xStunAttrType_IceControlling));
}

/* ───────────────────── Padding ───────────────────── */

TEST_F(StunAttrTest, PaddingAlignment) {
  /* USERNAME with odd length should be padded to 4-byte boundary */
  ASSERT_EQ(xStunAttrWriteUsername(&writer, "ab", "cd"), xErrno_Ok);
  /* "ab:cd" = 5 bytes, padded to 8 */
  size_t expected_size = XSTUN_ATTR_HEADER_SIZE + 8; /* 4 + 8 = 12 */
  EXPECT_EQ(writer.pos, expected_size);
}

/* ───────────────────── TURN Attributes ───────────────────── */

TEST_F(StunAttrTest, LifetimeRoundTrip) {
  ASSERT_EQ(xStunAttrWriteLifetime(&writer, 600), xErrno_Ok);

  size_t total;
  FinalizeMsg(&total);

  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_Lifetime);
  EXPECT_EQ(xReadU32BE(attr.value), 600u);
}

TEST_F(StunAttrTest, RequestedTransportRoundTrip) {
  ASSERT_EQ(xStunAttrWriteRequestedTransport(&writer, XTURN_TRANSPORT_UDP),
            xErrno_Ok);

  size_t total;
  FinalizeMsg(&total);

  xStunMsg decoded;
  ASSERT_EQ(xStunMsgDecode(&decoded, msg_buf, total), xErrno_Ok);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &decoded);
  xStunAttr attr;
  ASSERT_TRUE(xStunAttrIterNext(&iter, &attr));
  EXPECT_EQ(attr.type, xStunAttrType_RequestedTransport);
  EXPECT_EQ(xReadU32BE(attr.value), XTURN_TRANSPORT_UDP);
}
