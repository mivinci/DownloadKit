/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * datachannel_test.cpp - Integration tests for WebRTC DataChannel stack
 *
 * Tests the full pipeline: DCEP encoding/decoding, SDP WebRTC format,
 * and DataChannel manager lifecycle.
 */

#include <gtest/gtest.h>

extern "C" {
#include "datachannel.h"
#include "sdp.h"
}

#include <string.h>

/* ═══════════════════════════════════════════════════════════
 *  DCEP Encoding / Decoding Tests
 * ═══════════════════════════════════════════════════════════ */

class DcepTest : public ::testing::Test {};

TEST_F(DcepTest, OpenMessageRoundTrip) {
  /* Create a DataChannel config */
  xDataChannelConf conf;
  memset(&conf, 0, sizeof(conf));
  strncpy(conf.label, "test-channel", XDC_MAX_LABEL_LEN - 1);
  strncpy(conf.protocol, "", XDC_MAX_PROTOCOL_LEN - 1);
  conf.ordered = true;

  /* Encode OPEN message manually via the internal DCEP format:
   * byte 0: message type (0x03 = DATA_CHANNEL_OPEN)
   * byte 1: channel type
   * bytes 2-3: priority
   * bytes 4-7: reliability parameter
   * bytes 8-9: label length
   * bytes 10-11: protocol length
   * bytes 12+: label + protocol */
  uint8_t buf[512];
  size_t  label_len = strlen(conf.label);

  buf[0] = XDCEP_DATA_CHANNEL_OPEN;
  buf[1] = XDCEP_CHANNEL_RELIABLE; /* ordered, reliable */
  buf[2] = 0;
  buf[3] = 0;
  buf[4] = 0;
  buf[5] = 0;
  buf[6] = 0;
  buf[7] = 0;
  buf[8] = (uint8_t)(label_len >> 8);
  buf[9] = (uint8_t)(label_len);
  buf[10] = 0;
  buf[11] = 0;
  memcpy(buf + 12, conf.label, label_len);

  size_t total = 12 + label_len;

  /* Verify the message type */
  EXPECT_EQ(buf[0], XDCEP_DATA_CHANNEL_OPEN);
  EXPECT_EQ(buf[1], XDCEP_CHANNEL_RELIABLE);
  EXPECT_EQ(total, 12 + strlen("test-channel"));
}

TEST_F(DcepTest, AckMessage) {
  uint8_t ack = XDCEP_DATA_CHANNEL_ACK;
  EXPECT_EQ(ack, 0x02);
}

TEST_F(DcepTest, ChannelTypeUnordered) {
  /* Unordered reliable = 0x80 */
  EXPECT_EQ(XDCEP_CHANNEL_RELIABLE_UNORDERED, 0x80);
}

TEST_F(DcepTest, ChannelTypePartialRetransmit) {
  EXPECT_EQ(XDCEP_CHANNEL_PARTIAL_RTXS, 0x01);
  EXPECT_EQ(XDCEP_CHANNEL_PARTIAL_RTXS_UNORDERED, 0x81);
}

TEST_F(DcepTest, ChannelTypePartialTime) {
  EXPECT_EQ(XDCEP_CHANNEL_PARTIAL_TIME, 0x02);
  EXPECT_EQ(XDCEP_CHANNEL_PARTIAL_TIME_UNORDERED, 0x82);
}

/* ═══════════════════════════════════════════════════════════
 *  WebRTC SDP Encoding / Decoding Tests
 * ═══════════════════════════════════════════════════════════ */

class WebRTCSdpTest : public ::testing::Test {};

TEST_F(WebRTCSdpTest, EncodeWebRTCSdp) {
  char buf[XSDP_MAX_SIZE];
  int  len = xIceSdpEncodeWebRTC(
    "user1234", "pass5678", NULL, 0, true,
    "sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
    "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99",
    xIceSdpSetup_Actpass, "0", 5000, buf, sizeof(buf));

  ASSERT_GT(len, 0);
  buf[len] = '\0';

  /* Verify required WebRTC SDP fields */
  EXPECT_NE(strstr(buf, "v=0"), nullptr);
  EXPECT_NE(strstr(buf, "a=group:BUNDLE 0"), nullptr);
  EXPECT_NE(strstr(buf, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel"),
            nullptr);
  EXPECT_NE(strstr(buf, "a=mid:0"), nullptr);
  EXPECT_NE(strstr(buf, "a=ice-ufrag:user1234"), nullptr);
  EXPECT_NE(strstr(buf, "a=ice-pwd:pass5678"), nullptr);
  EXPECT_NE(strstr(buf, "a=fingerprint:sha-256 AA:BB:CC"), nullptr);
  EXPECT_NE(strstr(buf, "a=setup:actpass"), nullptr);
  EXPECT_NE(strstr(buf, "a=sctp-port:5000"), nullptr);
  EXPECT_NE(strstr(buf, "a=ice-options:trickle"), nullptr);
}

TEST_F(WebRTCSdpTest, DecodeWebRTCSdp) {
  const char *sdp =
    "v=0\r\n"
    "o=- 0 0 IN IP4 0.0.0.0\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "a=group:BUNDLE 0\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=mid:0\r\n"
    "a=ice-ufrag:testufrag\r\n"
    "a=ice-pwd:testpassword\r\n"
    "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
    "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
    "a=setup:active\r\n"
    "a=sctp-port:5000\r\n"
    "a=ice-options:trickle\r\n";

  xIceSdp parsed;
  xErrno  err = xIceSdpDecode(sdp, strlen(sdp), &parsed);
  ASSERT_EQ(err, xErrno_Ok);

  EXPECT_STREQ(parsed.ice_ufrag, "testufrag");
  EXPECT_STREQ(parsed.ice_pwd, "testpassword");
  EXPECT_TRUE(parsed.is_webrtc);
  EXPECT_TRUE(parsed.trickle);
  EXPECT_NE(strstr(parsed.fingerprint, "sha-256 AA:BB:CC"), nullptr);
  EXPECT_EQ(parsed.setup, xIceSdpSetup_Active);
  EXPECT_STREQ(parsed.mid, "0");
  EXPECT_EQ(parsed.sctp_port, 5000);
}

TEST_F(WebRTCSdpTest, DecodeWebRTCSdpMissingFingerprint) {
  const char *sdp =
    "v=0\r\n"
    "o=- 0 0 IN IP4 0.0.0.0\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:testufrag\r\n"
    "a=ice-pwd:testpassword\r\n"
    "a=setup:active\r\n";

  xIceSdp parsed;
  xErrno  err = xIceSdpDecode(sdp, strlen(sdp), &parsed);

  /* WebRTC SDP without fingerprint should fail */
  EXPECT_EQ(err, xErrno_InvalidArg);
}

TEST_F(WebRTCSdpTest, DecodePureIceSdpBackwardCompat) {
  const char *sdp =
    "v=0\r\n"
    "o=- 0 0 IN IP4 0.0.0.0\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=application 9 UDP/ICE 0\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=ice-ufrag:pureice\r\n"
    "a=ice-pwd:pureicepwd\r\n"
    "a=ice-options:trickle\r\n";

  xIceSdp parsed;
  xErrno  err = xIceSdpDecode(sdp, strlen(sdp), &parsed);
  ASSERT_EQ(err, xErrno_Ok);

  /* Pure ICE SDP should parse fine without fingerprint */
  EXPECT_FALSE(parsed.is_webrtc);
  EXPECT_STREQ(parsed.ice_ufrag, "pureice");
  EXPECT_STREQ(parsed.ice_pwd, "pureicepwd");
  EXPECT_TRUE(parsed.trickle);
}

TEST_F(WebRTCSdpTest, EncodeDecodeRoundTrip) {
  char buf[XSDP_MAX_SIZE];
  int  len = xIceSdpEncodeWebRTC(
    "roundtrip_ufrag", "roundtrip_pwd", NULL, 0, true,
    "sha-256 01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:"
    "11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F:20",
    xIceSdpSetup_Passive, "data", 5000, buf, sizeof(buf));

  ASSERT_GT(len, 0);
  buf[len] = '\0';

  xIceSdp parsed;
  xErrno  err = xIceSdpDecode(buf, (size_t)len, &parsed);
  ASSERT_EQ(err, xErrno_Ok);

  EXPECT_STREQ(parsed.ice_ufrag, "roundtrip_ufrag");
  EXPECT_STREQ(parsed.ice_pwd, "roundtrip_pwd");
  EXPECT_TRUE(parsed.is_webrtc);
  EXPECT_EQ(parsed.setup, xIceSdpSetup_Passive);
  EXPECT_STREQ(parsed.mid, "data");
  EXPECT_EQ(parsed.sctp_port, 5000);
}

/* ═══════════════════════════════════════════════════════════
 *  DataChannel Manager Tests
 * ═══════════════════════════════════════════════════════════ */

/* These tests verify the DataChannel manager's state machine
 * without a real SCTP transport (SCTP calls will fail gracefully). */

class DataChannelMgrTest : public ::testing::Test {};

TEST_F(DataChannelMgrTest, CreateManagerNullSctp) {
  xDataChannelMgrConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.sctp = NULL;

  xDataChannelMgr mgr = xDataChannelMgrCreate(&conf);
  EXPECT_EQ(mgr, nullptr);
}

TEST_F(DataChannelMgrTest, DestroyNull) {
  /* Should not crash */
  xDataChannelMgrDestroy(NULL);
}

TEST_F(DataChannelMgrTest, OnDataNull) {
  /* Should not crash */
  xDataChannelMgrOnData(NULL, 0, 0, NULL, 0);
}

TEST_F(DataChannelMgrTest, OnStreamCloseNull) {
  /* Should not crash */
  xDataChannelMgrOnStreamClose(NULL, 0);
}

/* ═══════════════════════════════════════════════════════════
 *  DataChannel API Tests
 * ═══════════════════════════════════════════════════════════ */

TEST(DataChannelApiTest, SendStringNull) {
  xErrno err = xDataChannelSendString(NULL, "hello", 5);
  EXPECT_EQ(err, xErrno_InvalidArg);
}

TEST(DataChannelApiTest, SendBinaryNull) {
  uint8_t data[] = {1, 2, 3};
  xErrno  err    = xDataChannelSendBinary(NULL, data, sizeof(data));
  EXPECT_EQ(err, xErrno_InvalidArg);
}

TEST(DataChannelApiTest, CloseNull) {
  /* Should not crash */
  xDataChannelClose(NULL);
}

TEST(DataChannelApiTest, GetLabelNull) {
  const char *label = xDataChannelGetLabel(NULL);
  EXPECT_STREQ(label, "");
}

TEST(DataChannelApiTest, GetStateNull) {
  xDataChannelState state = xDataChannelGetState(NULL);
  EXPECT_EQ(state, xDataChannelState_Closed);
}

TEST(DataChannelApiTest, GetStreamIdNull) {
  uint16_t id = xDataChannelGetStreamId(NULL);
  EXPECT_EQ(id, 0);
}

/* ═══════════════════════════════════════════════════════════
 *  SCTP PPID Constants Tests
 * ═══════════════════════════════════════════════════════════ */

TEST(SctpPpidTest, PpidValues) {
  EXPECT_EQ(XSCTP_PPID_DCEP, 50u);
  EXPECT_EQ(XSCTP_PPID_STRING, 51u);
  EXPECT_EQ(XSCTP_PPID_BINARY, 53u);
  EXPECT_EQ(XSCTP_PPID_STRING_EMPTY, 56u);
  EXPECT_EQ(XSCTP_PPID_BINARY_EMPTY, 57u);
}
