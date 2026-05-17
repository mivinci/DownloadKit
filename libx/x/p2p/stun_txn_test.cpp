/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * stun_txn_test.cpp - Unit tests for STUN transaction management
 */

#include <gtest/gtest.h>

extern "C" {
#include "stun_msg.h"
#include "stun_txn.h"
}

#include <arpa/inet.h>

/* ───────────────────── Test Helpers ───────────────────── */

struct SendRecord {
  uint8_t                 data[XSTUN_MAX_MSG_SIZE];
  size_t                  len;
  struct sockaddr_storage dest;
  int                     call_count;
};

static SendRecord g_send;

static xErrno mock_send(const uint8_t *data, size_t len, const struct sockaddr *addr, void *arg) {
  (void)arg;
  if (len > sizeof(g_send.data)) return xErrno_NoMemory;
  memcpy(g_send.data, data, len);
  g_send.len = len;
  memcpy(&g_send.dest, addr, sizeof(struct sockaddr_in));
  g_send.call_count++;
  return xErrno_Ok;
}

static xErrno mock_send_fail(const uint8_t *data, size_t len, const struct sockaddr *addr,
                             void *arg) {
  (void)data;
  (void)len;
  (void)addr;
  (void)arg;
  return xErrno_SysError;
}

struct CompletionRecord {
  bool     called;
  bool     timed_out;
  xStunMsg msg;
};

static CompletionRecord g_complete;

static void mock_on_complete(const xStunMsg *msg, const struct sockaddr *addr, void *arg) {
  (void)addr;
  (void)arg;
  g_complete.called = true;
  if (msg) {
    g_complete.timed_out = false;
    g_complete.msg       = *msg;
  } else {
    g_complete.timed_out = true;
  }
}

class StunTxnTest : public ::testing::Test {
protected:
  xEventLoop         loop;
  xStunTxnMgr        mgr;
  struct sockaddr_in dest;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
    xStunTxnMgrInit(&mgr, loop);

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(3478);
    inet_pton(AF_INET, "198.51.100.1", &dest.sin_addr);

    memset(&g_send, 0, sizeof(g_send));
    memset(&g_complete, 0, sizeof(g_complete));
  }

  void TearDown() override {
    xStunTxnMgrDestroy(&mgr);
    xEventLoopDestroy(loop);
  }
};

/* ───────────────────── Tests ───────────────────── */

TEST_F(StunTxnTest, SendCreatesTransaction) {
  xErrno err = xStunTxnMgrSend(&mgr, xStunMsgType_BindingRequest, NULL, 0, (struct sockaddr *)&dest,
                               mock_send, NULL, mock_on_complete, NULL);
  ASSERT_EQ(err, xErrno_Ok);
  EXPECT_EQ(g_send.call_count, 1);
  EXPECT_EQ(mgr.count, 1);

  /* Verify the sent data is a valid STUN message */
  EXPECT_TRUE(xStunMsgIsStun(g_send.data, g_send.len));
}

TEST_F(StunTxnTest, ResponseMatchesTransaction) {
  xErrno err = xStunTxnMgrSend(&mgr, xStunMsgType_BindingRequest, NULL, 0, (struct sockaddr *)&dest,
                               mock_send, NULL, mock_on_complete, NULL);
  ASSERT_EQ(err, xErrno_Ok);

  /* Decode the sent request to get the txn_id */
  xStunMsg sent_msg;
  ASSERT_EQ(xStunMsgDecode(&sent_msg, g_send.data, g_send.len), xErrno_Ok);

  /* Build a response with the same txn_id */
  xStunMsg response;
  xStunMsgInit(&response, xStunMsgType_BindingResponse, sent_msg.txn_id);
  uint8_t resp_buf[64];
  int     resp_len = xStunMsgEncode(&response, resp_buf, sizeof(resp_buf));
  ASSERT_GT(resp_len, 0);

  /* Decode the response */
  xStunMsg decoded_resp;
  ASSERT_EQ(xStunMsgDecode(&decoded_resp, resp_buf, resp_len), xErrno_Ok);

  /* Feed it to the manager */
  bool matched =
    xStunTxnMgrOnResponse(&mgr, &decoded_resp, resp_buf, resp_len, (struct sockaddr *)&dest);
  EXPECT_TRUE(matched);
  EXPECT_TRUE(g_complete.called);
  EXPECT_FALSE(g_complete.timed_out);
  EXPECT_EQ(mgr.count, 0);
}

TEST_F(StunTxnTest, UnmatchedResponseDiscarded) {
  xErrno err = xStunTxnMgrSend(&mgr, xStunMsgType_BindingRequest, NULL, 0, (struct sockaddr *)&dest,
                               mock_send, NULL, mock_on_complete, NULL);
  ASSERT_EQ(err, xErrno_Ok);

  /* Build a response with a different txn_id */
  uint8_t fake_txn_id[XSTUN_TXN_ID_SIZE];
  memset(fake_txn_id, 0xFF, XSTUN_TXN_ID_SIZE);

  xStunMsg response;
  xStunMsgInit(&response, xStunMsgType_BindingResponse, fake_txn_id);
  uint8_t resp_buf[64];
  int     resp_len = xStunMsgEncode(&response, resp_buf, sizeof(resp_buf));
  ASSERT_GT(resp_len, 0);

  xStunMsg decoded_resp;
  ASSERT_EQ(xStunMsgDecode(&decoded_resp, resp_buf, resp_len), xErrno_Ok);

  bool matched =
    xStunTxnMgrOnResponse(&mgr, &decoded_resp, resp_buf, resp_len, (struct sockaddr *)&dest);
  EXPECT_FALSE(matched);
  EXPECT_FALSE(g_complete.called);
  EXPECT_EQ(mgr.count, 1); /* Transaction still pending */
}

TEST_F(StunTxnTest, CancelAllCleansUp) {
  /* Create two transactions */
  ASSERT_EQ(xStunTxnMgrSend(&mgr, xStunMsgType_BindingRequest, NULL, 0, (struct sockaddr *)&dest,
                            mock_send, NULL, mock_on_complete, NULL),
            xErrno_Ok);
  ASSERT_EQ(xStunTxnMgrSend(&mgr, xStunMsgType_BindingRequest, NULL, 0, (struct sockaddr *)&dest,
                            mock_send, NULL, mock_on_complete, NULL),
            xErrno_Ok);
  EXPECT_EQ(mgr.count, 2);

  xStunTxnMgrCancelAll(&mgr);
  EXPECT_EQ(mgr.count, 0);
}

TEST_F(StunTxnTest, SendFailReturnsError) {
  xErrno err = xStunTxnMgrSend(&mgr, xStunMsgType_BindingRequest, NULL, 0, (struct sockaddr *)&dest,
                               mock_send_fail, NULL, mock_on_complete, NULL);
  EXPECT_NE(err, xErrno_Ok);
  EXPECT_EQ(mgr.count, 0);
}

TEST_F(StunTxnTest, SendRawWorks) {
  /* Build a message manually */
  uint8_t  txn_id[XSTUN_TXN_ID_SIZE] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  xStunMsg msg;
  xStunMsgInit(&msg, xStunMsgType_BindingRequest, txn_id);
  uint8_t msg_buf[64];
  int     msg_len = xStunMsgEncode(&msg, msg_buf, sizeof(msg_buf));
  ASSERT_GT(msg_len, 0);

  xErrno err = xStunTxnMgrSendRaw(&mgr, msg_buf, msg_len, (struct sockaddr *)&dest, mock_send, NULL,
                                  mock_on_complete, NULL);
  ASSERT_EQ(err, xErrno_Ok);
  EXPECT_EQ(mgr.count, 1);

  /* Verify the txn_id was preserved */
  EXPECT_EQ(memcmp(mgr.txns[0]->txn_id, txn_id, XSTUN_TXN_ID_SIZE), 0);
}

TEST_F(StunTxnTest, MultipleTransactionsMultiplex) {
  /* Send 3 transactions */
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(xStunTxnMgrSend(&mgr, xStunMsgType_BindingRequest, NULL, 0, (struct sockaddr *)&dest,
                              mock_send, NULL, mock_on_complete, NULL),
              xErrno_Ok);
  }
  EXPECT_EQ(mgr.count, 3);

  /* Respond to the second one */
  xStunMsg response;
  xStunMsgInit(&response, xStunMsgType_BindingResponse, mgr.txns[1]->txn_id);
  uint8_t resp_buf[64];
  int     resp_len = xStunMsgEncode(&response, resp_buf, sizeof(resp_buf));
  ASSERT_GT(resp_len, 0);

  xStunMsg decoded_resp;
  ASSERT_EQ(xStunMsgDecode(&decoded_resp, resp_buf, resp_len), xErrno_Ok);

  bool matched =
    xStunTxnMgrOnResponse(&mgr, &decoded_resp, resp_buf, resp_len, (struct sockaddr *)&dest);
  EXPECT_TRUE(matched);
  EXPECT_EQ(mgr.count, 2); /* One removed, two remaining */
}
