/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * turn_test.cpp - Unit tests for TURN client and ChannelData
 */

#include <gtest/gtest.h>

extern "C" {
#include "stun_attr.h"
#include "stun_msg.h"
#include "turn_channel.h"
#include "turn_client.h"
}

#include <arpa/inet.h>

/* ───────────────────── ChannelData Tests ───────────────────── */

TEST(TurnChannelTest, EncodeDecodeRoundTrip) {
  uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  uint8_t buf[64];

  int encoded = xTurnChannelDataEncode(0x4001, data, 5, buf, sizeof(buf));
  ASSERT_GT(encoded, 0);
  /* 4 header + 5 data + 3 padding = 12 */
  EXPECT_EQ(encoded, 12);

  uint16_t       channel;
  const uint8_t *out_data;
  uint16_t       out_len;
  ASSERT_EQ(xTurnChannelDataDecode(buf, encoded, &channel, &out_data, &out_len),
            xErrno_Ok);
  EXPECT_EQ(channel, 0x4001);
  EXPECT_EQ(out_len, 5);
  EXPECT_EQ(memcmp(out_data, data, 5), 0);
}

TEST(TurnChannelTest, DecodeInvalidChannel) {
  uint8_t        buf[8] = {0x00, 0x01, 0x00, 0x01, 0xAA, 0, 0, 0};
  uint16_t       channel;
  const uint8_t *data;
  uint16_t       data_len;
  /* Channel 0x0001 is not in valid range */
  EXPECT_NE(
    xTurnChannelDataDecode(buf, sizeof(buf), &channel, &data, &data_len),
    xErrno_Ok);
}

TEST(TurnChannelTest, DecodeTooShort) {
  uint8_t        buf[2] = {0x40, 0x01};
  uint16_t       channel;
  const uint8_t *data;
  uint16_t       data_len;
  EXPECT_NE(
    xTurnChannelDataDecode(buf, sizeof(buf), &channel, &data, &data_len),
    xErrno_Ok);
}

TEST(TurnChannelTest, IsChannelData) {
  EXPECT_TRUE(xTurnIsChannelData(0x40));
  EXPECT_TRUE(xTurnIsChannelData(0x7F));
  EXPECT_FALSE(xTurnIsChannelData(0x00));
  EXPECT_FALSE(xTurnIsChannelData(0x80));
  EXPECT_FALSE(xTurnIsChannelData(0x3F));
}

TEST(TurnChannelTest, EncodeBufferTooSmall) {
  uint8_t data[10] = {};
  uint8_t buf[8]; /* Too small for 4 + 12 */
  int     encoded = xTurnChannelDataEncode(0x4001, data, 10, buf, sizeof(buf));
  EXPECT_EQ(encoded, -1);
}

/* ───────────────────── TURN Client Tests ───────────────────── */

struct TurnSendRecord {
  uint8_t                 data[XSTUN_MAX_MSG_SIZE];
  size_t                  len;
  struct sockaddr_storage dest;
  int                     call_count;
};

static TurnSendRecord g_turn_send;

static xErrno turn_mock_send(const uint8_t *data, size_t len,
                             const struct sockaddr *addr, void *arg) {
  (void)arg;
  if (len > sizeof(g_turn_send.data)) return xErrno_NoMemory;
  memcpy(g_turn_send.data, data, len);
  g_turn_send.len = len;
  memcpy(&g_turn_send.dest, addr, sizeof(struct sockaddr_in));
  g_turn_send.call_count++;
  return xErrno_Ok;
}

struct TurnAllocResult {
  bool     allocated;
  bool     failed;
  uint32_t lifetime;
};

static TurnAllocResult g_alloc_result;

static void turn_on_allocated(const struct sockaddr *relayed_addr,
                              const struct sockaddr *mapped_addr,
                              uint32_t lifetime, void *arg) {
  (void)relayed_addr;
  (void)mapped_addr;
  (void)arg;
  g_alloc_result.allocated = true;
  g_alloc_result.lifetime  = lifetime;
}

static void turn_on_failed(xErrno err, void *arg) {
  (void)err;
  (void)arg;
  g_alloc_result.failed = true;
}

class TurnClientTest : public ::testing::Test {
protected:
  xEventLoop         loop;
  xTurnClient        client;
  struct sockaddr_in server;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port   = htons(3478);
    inet_pton(AF_INET, "198.51.100.1", &server.sin_addr);

    xTurnConfig config;
    memset(&config, 0, sizeof(config));
    memcpy(&config.server, &server, sizeof(server));
    strncpy(config.username, "testuser", sizeof(config.username) - 1);
    strncpy(config.password, "testpass", sizeof(config.password) - 1);
    config.send_fn      = turn_mock_send;
    config.send_arg     = NULL;
    config.on_allocated = turn_on_allocated;
    config.on_failed    = turn_on_failed;
    config.ctx          = NULL;

    xTurnClientInit(&client, loop, &config);

    memset(&g_turn_send, 0, sizeof(g_turn_send));
    memset(&g_alloc_result, 0, sizeof(g_alloc_result));
  }

  void TearDown() override {
    xTurnClientDestroy(&client);
    xEventLoopDestroy(loop);
  }
};

TEST_F(TurnClientTest, AllocateSendsRequest) {
  xErrno err = xTurnClientAllocate(&client);
  ASSERT_EQ(err, xErrno_Ok);
  EXPECT_EQ(g_turn_send.call_count, 1);
  EXPECT_TRUE(xStunMsgIsStun(g_turn_send.data, g_turn_send.len));

  /* Verify it's an Allocate Request */
  xStunMsg msg;
  ASSERT_EQ(xStunMsgDecode(&msg, g_turn_send.data, g_turn_send.len), xErrno_Ok);
  EXPECT_EQ(msg.type, xStunMsgType_AllocateRequest);

  /* Verify REQUESTED-TRANSPORT attribute */
  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &msg);
  xStunAttr attr;
  bool      found_transport = false;
  while (xStunAttrIterNext(&iter, &attr)) {
    if (attr.type == xStunAttrType_RequestedTransport) {
      EXPECT_EQ(xReadU32BE(attr.value), XTURN_TRANSPORT_UDP);
      found_transport = true;
    }
  }
  EXPECT_TRUE(found_transport);
}

TEST_F(TurnClientTest, AllocateDoubleCallFails) {
  ASSERT_EQ(xTurnClientAllocate(&client), xErrno_Ok);
  EXPECT_NE(xTurnClientAllocate(&client), xErrno_Ok);
}

TEST_F(TurnClientTest, ChannelDataSendViaChannel) {
  /* Manually set state to Allocated */
  client.state = xTurnState_Allocated;

  struct sockaddr_in peer;
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port   = htons(5000);
  inet_pton(AF_INET, "10.0.0.1", &peer.sin_addr);

  /* Bind a channel */
  int ch = xTurnClientChannelBind(&client, (struct sockaddr *)&peer);
  ASSERT_GE(ch, XTURN_CHANNEL_MIN);
  ASSERT_LE(ch, XTURN_CHANNEL_MAX);

  /* Reset send counter */
  g_turn_send.call_count = 0;

  /* Send data — should use ChannelData */
  uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
  xErrno  err =
    xTurnClientSendData(&client, (struct sockaddr *)&peer, data, sizeof(data));
  ASSERT_EQ(err, xErrno_Ok);
  EXPECT_EQ(g_turn_send.call_count, 1);

  /* Verify it's ChannelData (first byte in 0x40-0x7F range) */
  EXPECT_TRUE(xTurnIsChannelData(g_turn_send.data[0]));
}

TEST_F(TurnClientTest, SendDataFallbackToIndication) {
  client.state = xTurnState_Allocated;

  struct sockaddr_in peer;
  memset(&peer, 0, sizeof(peer));
  peer.sin_family = AF_INET;
  peer.sin_port   = htons(5000);
  inet_pton(AF_INET, "10.0.0.1", &peer.sin_addr);

  /* No channel bound — should use Send Indication */
  uint8_t data[] = {0x01, 0x02};
  xErrno  err =
    xTurnClientSendData(&client, (struct sockaddr *)&peer, data, sizeof(data));
  ASSERT_EQ(err, xErrno_Ok);

  /* Verify it's a STUN message (Send Indication) */
  EXPECT_TRUE(xStunMsgIsStun(g_turn_send.data, g_turn_send.len));
  xStunMsg msg;
  ASSERT_EQ(xStunMsgDecode(&msg, g_turn_send.data, g_turn_send.len), xErrno_Ok);
  EXPECT_EQ(msg.type, xStunMsgType_SendIndication);
}

TEST_F(TurnClientTest, AllocateWith401ThenRetryWithCredentials) {
  /*
   * Simulate the 401 Unauthorized flow:
   * 1. Client sends Allocate without credentials
   * 2. Server responds with 401 + realm + nonce
   * 3. Client retries with credentials (MESSAGE-INTEGRITY)
   */
  ASSERT_EQ(xTurnClientAllocate(&client), xErrno_Ok);
  EXPECT_EQ(g_turn_send.call_count, 1);

  /* Decode the first request to get txn_id */
  xStunMsg req1;
  ASSERT_EQ(xStunMsgDecode(&req1, g_turn_send.data, g_turn_send.len),
            xErrno_Ok);

  /* Build a 401 error response */
  uint8_t  resp_buf[512];
  xStunMsg resp;
  xStunMsgInit(&resp, xStunMsgType_AllocateErrorResponse, req1.txn_id);
  xStunMsgEncode(&resp, resp_buf, sizeof(resp_buf));

  xStunAttrWriter w;
  xStunAttrWriterInit(&w, resp_buf + XSTUN_HEADER_SIZE,
                      sizeof(resp_buf) - XSTUN_HEADER_SIZE);
  xStunAttrWriteErrorCode(&w, 401, "Unauthorized");
  xStunAttrWriteRealm(&w, "example.org");
  xStunAttrWriteNonce(&w, "testnonce123");
  xWriteU16BE(resp_buf + 2, (uint16_t)w.pos);
  size_t resp_len = XSTUN_HEADER_SIZE + w.pos;

  /* Feed the 401 response to the client */
  xStunMsg resp_decoded;
  ASSERT_EQ(xStunMsgDecode(&resp_decoded, resp_buf, resp_len), xErrno_Ok);
  xTurnClientOnMessage(&client, &resp_decoded, resp_buf, resp_len,
                       (struct sockaddr *)&server);

  /* Client should have retried with credentials */
  EXPECT_EQ(g_turn_send.call_count, 2);

  /* Verify the retry has USERNAME, REALM, NONCE, MESSAGE-INTEGRITY */
  xStunMsg req2;
  ASSERT_EQ(xStunMsgDecode(&req2, g_turn_send.data, g_turn_send.len),
            xErrno_Ok);
  EXPECT_EQ(req2.type, xStunMsgType_AllocateRequest);

  xStunAttrIter iter;
  xStunAttrIterInit(&iter, &req2);
  xStunAttr attr;
  bool      has_username = false, has_realm = false;
  bool      has_nonce = false, has_integrity = false;
  while (xStunAttrIterNext(&iter, &attr)) {
    if (attr.type == xStunAttrType_Username) has_username = true;
    if (attr.type == xStunAttrType_Realm) has_realm = true;
    if (attr.type == xStunAttrType_Nonce) has_nonce = true;
    if (attr.type == xStunAttrType_MessageIntegrity) has_integrity = true;
  }
  EXPECT_TRUE(has_username);
  EXPECT_TRUE(has_realm);
  EXPECT_TRUE(has_nonce);
  EXPECT_TRUE(has_integrity);

  /* Verify realm and nonce were stored */
  EXPECT_STREQ(client.realm, "example.org");
  EXPECT_STREQ(client.nonce, "testnonce123");
  EXPECT_TRUE(client.has_credentials);
}

TEST_F(TurnClientTest, RefreshTimerScheduledOnAllocateSuccess) {
  /*
   * Simulate a successful Allocate response and verify
   * that the refresh timer is scheduled.
   */
  ASSERT_EQ(xTurnClientAllocate(&client), xErrno_Ok);

  /* Decode request for txn_id */
  xStunMsg req;
  ASSERT_EQ(xStunMsgDecode(&req, g_turn_send.data, g_turn_send.len), xErrno_Ok);

  /* Build a success response with RELAYED-ADDRESS and LIFETIME */
  uint8_t  resp_buf[512];
  xStunMsg resp;
  xStunMsgInit(&resp, xStunMsgType_AllocateResponse, req.txn_id);
  xStunMsgEncode(&resp, resp_buf, sizeof(resp_buf));

  xStunAttrWriter w;
  xStunAttrWriterInit(&w, resp_buf + XSTUN_HEADER_SIZE,
                      sizeof(resp_buf) - XSTUN_HEADER_SIZE);

  /* XOR-RELAYED-ADDRESS */
  struct sockaddr_in relay;
  memset(&relay, 0, sizeof(relay));
  relay.sin_family = AF_INET;
  relay.sin_port   = htons(49152);
  inet_pton(AF_INET, "198.51.100.1", &relay.sin_addr);
  xStunAttrWriteXorMappedAddress(&w, (struct sockaddr *)&relay, req.txn_id);
  /* Patch the type to XOR-RELAYED-ADDRESS (0x0016) */
  uint8_t *attr_start = resp_buf + XSTUN_HEADER_SIZE;
  xWriteU16BE(attr_start, (uint16_t)xStunAttrType_XorRelayedAddress);

  /* LIFETIME = 600 seconds */
  xStunAttrWriteLifetime(&w, 600);

  xWriteU16BE(resp_buf + 2, (uint16_t)w.pos);
  size_t resp_len = XSTUN_HEADER_SIZE + w.pos;

  /* Feed the success response */
  xStunMsg resp_decoded;
  ASSERT_EQ(xStunMsgDecode(&resp_decoded, resp_buf, resp_len), xErrno_Ok);
  xTurnClientOnMessage(&client, &resp_decoded, resp_buf, resp_len,
                       (struct sockaddr *)&server);

  /* Verify allocation succeeded */
  EXPECT_TRUE(g_alloc_result.allocated);
  EXPECT_EQ(g_alloc_result.lifetime, 600u);
  EXPECT_EQ(client.state, xTurnState_Allocated);
  EXPECT_EQ(client.lifetime, 600u);

  /* Verify refresh timer was scheduled */
  EXPECT_NE(client.refresh_timer, nullptr);
}
