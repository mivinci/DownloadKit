/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * stun_msg_test.cpp - Unit tests for STUN message encoding / decoding
 */

#include <gtest/gtest.h>

extern "C" {
#include "stun_msg.h"
}

class StunMsgTest : public ::testing::Test {
protected:
  uint8_t txn_id[XSTUN_TXN_ID_SIZE];
  uint8_t buf[256];

  void SetUp() override {
    for (int i = 0; i < XSTUN_TXN_ID_SIZE; i++) {
      txn_id[i] = (uint8_t)(0xA0 + i);
    }
    memset(buf, 0, sizeof(buf));
  }
};

TEST_F(StunMsgTest, EncodeDecodeRoundTrip) {
  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);

  int encoded = xStunMsgEncode(&msg, buf, sizeof(buf));
  ASSERT_EQ(encoded, XSTUN_HEADER_SIZE);

  xStunMsg decoded;
  xErrno   err = xStunMsgDecode(&decoded, buf, encoded);
  ASSERT_EQ(err, xErrno_Ok);
  EXPECT_EQ(decoded.type, xStunMsgType_BindingRequest);
  EXPECT_EQ(decoded.length, 0);
  EXPECT_EQ(decoded.attrs_len, 0);
  EXPECT_EQ(decoded.attrs, nullptr);
  EXPECT_EQ(memcmp(decoded.txn_id, txn_id, XSTUN_TXN_ID_SIZE), 0);
}

TEST_F(StunMsgTest, EncodeDecodeWithPayload) {
  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingResponse, txn_id);

  /* Simulate some attribute payload */
  uint8_t attrs[] = {0x00, 0x20, 0x00, 0x08,  /* XOR-MAPPED-ADDRESS TLV */
                     0x00, 0x01, 0x21, 0x12,  /* family + xor port     */
                     0x21, 0x12, 0xA4, 0x42}; /* xor address           */
  msg.attrs       = attrs;
  msg.attrs_len   = sizeof(attrs);

  int encoded = xStunMsgEncode(&msg, buf, sizeof(buf));
  ASSERT_EQ(encoded, XSTUN_HEADER_SIZE + (int)sizeof(attrs));

  xStunMsg decoded;
  xErrno   err = xStunMsgDecode(&decoded, buf, encoded);
  ASSERT_EQ(err, xErrno_Ok);
  EXPECT_EQ(decoded.type, xStunMsgType_BindingResponse);
  EXPECT_EQ(decoded.attrs_len, sizeof(attrs));
  EXPECT_EQ(memcmp(decoded.attrs, attrs, sizeof(attrs)), 0);
}

TEST_F(StunMsgTest, DecodeBufferTooSmall) {
  xStunMsg msg;
  uint8_t  small_buf[10] = {0};
  xErrno   err           = xStunMsgDecode(&msg, small_buf, sizeof(small_buf));
  EXPECT_NE(err, xErrno_Ok);
}

TEST_F(StunMsgTest, DecodeInvalidMagicCookie) {
  /* Build a valid-looking header but with wrong magic cookie */
  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);
  xStunMsgEncode(&msg, buf, sizeof(buf));

  /* Corrupt the magic cookie */
  buf[4] = 0xFF;

  xStunMsg decoded;
  xErrno   err = xStunMsgDecode(&decoded, buf, XSTUN_HEADER_SIZE);
  EXPECT_NE(err, xErrno_Ok);
}

TEST_F(StunMsgTest, DecodeFirstTwoBitsNotZero) {
  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);
  xStunMsgEncode(&msg, buf, sizeof(buf));

  /* Set first two bits to non-zero */
  buf[0] |= 0x80;

  xStunMsg decoded;
  xErrno   err = xStunMsgDecode(&decoded, buf, XSTUN_HEADER_SIZE);
  EXPECT_NE(err, xErrno_Ok);
}

TEST_F(StunMsgTest, DecodeLengthExceedsBuffer) {
  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);
  xStunMsgEncode(&msg, buf, sizeof(buf));

  /* Set length field to something larger than remaining buffer */
  xWriteU16BE(buf + 2, 100);

  xStunMsg decoded;
  xErrno   err = xStunMsgDecode(&decoded, buf, XSTUN_HEADER_SIZE);
  EXPECT_NE(err, xErrno_Ok);
}

TEST_F(StunMsgTest, EncodeBufferTooSmall) {
  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);

  uint8_t small_buf[10];
  int     result = xStunMsgEncode(&msg, small_buf, sizeof(small_buf));
  EXPECT_EQ(result, -1);
}

TEST_F(StunMsgTest, EncodeNullArgs) {
  EXPECT_EQ(xStunMsgEncode(nullptr, buf, sizeof(buf)), -1);

  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);
  EXPECT_EQ(xStunMsgEncode(&msg, nullptr, sizeof(buf)), -1);
}

TEST_F(StunMsgTest, IsStunValid) {
  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);
  xStunMsgEncode(&msg, buf, sizeof(buf));

  EXPECT_TRUE(xStunMsgIsStun(buf, XSTUN_HEADER_SIZE));
}

TEST_F(StunMsgTest, IsStunInvalid) {
  /* Too short */
  EXPECT_FALSE(xStunMsgIsStun(buf, 5));

  /* NULL buffer */
  EXPECT_FALSE(xStunMsgIsStun(nullptr, 20));

  /* First two bits set */
  buf[0] = 0xC0;
  EXPECT_FALSE(xStunMsgIsStun(buf, XSTUN_HEADER_SIZE));

  /* Wrong magic cookie */
  memset(buf, 0, sizeof(buf));
  buf[4] = 0xFF;
  EXPECT_FALSE(xStunMsgIsStun(buf, XSTUN_HEADER_SIZE));
}

TEST_F(StunMsgTest, IsStunChannelData) {
  /* ChannelData starts with 0x40-0x7F — should NOT be detected as STUN */
  buf[0] = 0x40;
  EXPECT_FALSE(xStunMsgIsStun(buf, XSTUN_HEADER_SIZE));
}

TEST_F(StunMsgTest, MessageClassHelpers) {
  EXPECT_TRUE(xStunMsgIsRequest(xStunMsgType_BindingRequest));
  EXPECT_FALSE(xStunMsgIsRequest(xStunMsgType_BindingResponse));

  EXPECT_TRUE(xStunMsgIsIndication(xStunMsgType_BindingIndication));
  EXPECT_FALSE(xStunMsgIsIndication(xStunMsgType_BindingRequest));

  EXPECT_TRUE(xStunMsgIsSuccessResponse(xStunMsgType_BindingResponse));
  EXPECT_FALSE(xStunMsgIsSuccessResponse(xStunMsgType_BindingErrorResponse));

  EXPECT_TRUE(xStunMsgIsErrorResponse(xStunMsgType_BindingErrorResponse));
  EXPECT_FALSE(xStunMsgIsErrorResponse(xStunMsgType_BindingResponse));
}
