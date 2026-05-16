/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * dtls_transport_test.cpp - Unit tests for DTLS transport layer
 *
 * Covers:
 *   - Create / Destroy lifecycle
 *   - Fingerprint generation, formatting, parsing
 *   - Role resolution (Active / Passive / Actpass)
 *   - Active ↔ Passive handshake (loopback)
 *   - Encrypted data send / receive after handshake
 *   - Handshake timeout
 *   - Remote fingerprint verification (correct & wrong)
 *   - NULL-safety of all public APIs
 */

#include <gtest/gtest.h>

extern "C" {
#include "dtls_transport.h"
#include <x/base/event.h>
}

#include <cstring>
#include <vector>

/* ═══════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════ */

/**
 * Tick the event loop for up to @p total_ms, calling xEventWait in
 * small increments so timers and callbacks fire promptly.
 */
static void tick_loop(xEventLoop loop, int total_ms) {
  int       elapsed = 0;
  const int step    = 5;
  while (elapsed < total_ms) {
    xEventWait(loop, step);
    elapsed += step;
  }
}

/* ═══════════════════════════════════════════════════════════
 *  Fingerprint String Helpers
 * ═══════════════════════════════════════════════════════════ */

class DtlsFingerprintTest : public ::testing::Test {};

TEST_F(DtlsFingerprintTest, ParseValidFingerprint) {
  /* Build a known fingerprint string */
  const char *fp_str = "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
                       "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";

  uint8_t out[XDTLS_FINGERPRINT_SIZE];
  xErrno  err = xDtlsFingerprintFromStr(fp_str, out);
  ASSERT_EQ(err, xErrno_Ok);

  EXPECT_EQ(out[0], 0xAA);
  EXPECT_EQ(out[1], 0xBB);
  EXPECT_EQ(out[2], 0xCC);
  EXPECT_EQ(out[15], 0x99);
  EXPECT_EQ(out[16], 0xAA);
  EXPECT_EQ(out[31], 0x99);
}

TEST_F(DtlsFingerprintTest, ParseLowercaseFingerprint) {
  const char *fp_str = "aa:bb:cc:dd:ee:ff:00:11:22:33:44:55:66:77:88:99:"
                       "aa:bb:cc:dd:ee:ff:00:11:22:33:44:55:66:77:88:99";

  uint8_t out[XDTLS_FINGERPRINT_SIZE];
  xErrno  err = xDtlsFingerprintFromStr(fp_str, out);
  ASSERT_EQ(err, xErrno_Ok);

  EXPECT_EQ(out[0], 0xAA);
  EXPECT_EQ(out[5], 0xFF);
}

TEST_F(DtlsFingerprintTest, ParseTooShort) {
  const char *fp_str = "AA:BB:CC";
  uint8_t     out[XDTLS_FINGERPRINT_SIZE];
  xErrno      err = xDtlsFingerprintFromStr(fp_str, out);
  EXPECT_NE(err, xErrno_Ok);
}

TEST_F(DtlsFingerprintTest, ParseNull) {
  uint8_t out[XDTLS_FINGERPRINT_SIZE];
  EXPECT_NE(xDtlsFingerprintFromStr(NULL, out), xErrno_Ok);
  EXPECT_NE(xDtlsFingerprintFromStr("AA:BB", NULL), xErrno_Ok);
}

TEST_F(DtlsFingerprintTest, ParseInvalidChar) {
  /* 'GG' is not valid hex */
  const char *fp_str = "GG:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
                       "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";

  uint8_t out[XDTLS_FINGERPRINT_SIZE];
  xErrno  err = xDtlsFingerprintFromStr(fp_str, out);
  EXPECT_NE(err, xErrno_Ok);
}

/* ═══════════════════════════════════════════════════════════
 *  Create / Destroy Lifecycle
 * ═══════════════════════════════════════════════════════════ */

class DtlsLifecycleTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
  }
  void TearDown() override {
    if (loop) xEventLoopDestroy(loop);
  }
};

/* Dummy send function that does nothing */
static xErrno dummy_send(const uint8_t * /*data*/, size_t /*len*/, void * /*arg*/) {
  return xErrno_Ok;
}

TEST_F(DtlsLifecycleTest, CreateActiveAndDestroy) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(xDtlsTransportGetState(t), xDtlsState_New);
  EXPECT_EQ(xDtlsTransportGetRole(t), xDtlsRole_Active);

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsLifecycleTest, CreatePassiveAndDestroy) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Passive;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(xDtlsTransportGetState(t), xDtlsState_New);
  EXPECT_EQ(xDtlsTransportGetRole(t), xDtlsRole_Passive);

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsLifecycleTest, CreateActpassResolvesToPassive) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Actpass;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);
  /* Actpass should resolve to Passive */
  EXPECT_EQ(xDtlsTransportGetRole(t), xDtlsRole_Passive);

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsLifecycleTest, CreateNullConf) {
  xDtlsTransport t = xDtlsTransportCreate(NULL);
  EXPECT_EQ(t, nullptr);
}

TEST_F(DtlsLifecycleTest, CreateNullLoop) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = NULL;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  EXPECT_EQ(t, nullptr);
}

TEST_F(DtlsLifecycleTest, CreateNullSendFn) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = NULL;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  EXPECT_EQ(t, nullptr);
}

TEST_F(DtlsLifecycleTest, DestroyNull) {
  /* Should not crash */
  xDtlsTransportDestroy(NULL);
}

/* ═══════════════════════════════════════════════════════════
 *  Fingerprint Generation
 * ═══════════════════════════════════════════════════════════ */

class DtlsFingerprintGenTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
  }
  void TearDown() override {
    if (loop) xEventLoopDestroy(loop);
  }
};

TEST_F(DtlsFingerprintGenTest, GetFingerprintRaw) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  uint8_t fp[XDTLS_FINGERPRINT_SIZE];
  xErrno  err = xDtlsTransportGetFingerprint(t, fp);
  EXPECT_EQ(err, xErrno_Ok);

  /* Fingerprint should not be all zeros (that would mean cert gen failed) */
  bool all_zero = true;
  for (int i = 0; i < XDTLS_FINGERPRINT_SIZE; i++) {
    if (fp[i] != 0) {
      all_zero = false;
      break;
    }
  }
  EXPECT_FALSE(all_zero);

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsFingerprintGenTest, GetFingerprintStr) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Passive;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  char   fp_str[XDTLS_FINGERPRINT_STR_SIZE];
  xErrno err = xDtlsTransportGetFingerprintStr(t, fp_str);
  EXPECT_EQ(err, xErrno_Ok);

  /* Should be "XX:XX:XX:..." format, 95 chars (32*3-1) */
  size_t len = strlen(fp_str);
  EXPECT_EQ(len, (size_t)(XDTLS_FINGERPRINT_SIZE * 3 - 1));

  /* Every 3rd char (starting from index 2) should be ':' */
  for (size_t i = 2; i < len; i += 3) {
    EXPECT_EQ(fp_str[i], ':') << "Expected ':' at position " << i;
  }

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsFingerprintGenTest, FingerprintRoundTrip) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  /* Get raw fingerprint */
  uint8_t fp_raw[XDTLS_FINGERPRINT_SIZE];
  ASSERT_EQ(xDtlsTransportGetFingerprint(t, fp_raw), xErrno_Ok);

  /* Get string fingerprint */
  char fp_str[XDTLS_FINGERPRINT_STR_SIZE];
  ASSERT_EQ(xDtlsTransportGetFingerprintStr(t, fp_str), xErrno_Ok);

  /* Parse string back to raw */
  uint8_t fp_parsed[XDTLS_FINGERPRINT_SIZE];
  ASSERT_EQ(xDtlsFingerprintFromStr(fp_str, fp_parsed), xErrno_Ok);

  /* Should match */
  EXPECT_EQ(memcmp(fp_raw, fp_parsed, XDTLS_FINGERPRINT_SIZE), 0);

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsFingerprintGenTest, TwoTransportsDifferentFingerprints) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t1 = xDtlsTransportCreate(&conf);
  xDtlsTransport t2 = xDtlsTransportCreate(&conf);
  ASSERT_NE(t1, nullptr);
  ASSERT_NE(t2, nullptr);

  uint8_t fp1[XDTLS_FINGERPRINT_SIZE], fp2[XDTLS_FINGERPRINT_SIZE];
  ASSERT_EQ(xDtlsTransportGetFingerprint(t1, fp1), xErrno_Ok);
  ASSERT_EQ(xDtlsTransportGetFingerprint(t2, fp2), xErrno_Ok);

  /* Two independently generated certs should have different fingerprints */
  EXPECT_NE(memcmp(fp1, fp2, XDTLS_FINGERPRINT_SIZE), 0);

  xDtlsTransportDestroy(t1);
  xDtlsTransportDestroy(t2);
}

TEST_F(DtlsFingerprintGenTest, GetFingerprintNull) {
  EXPECT_NE(xDtlsTransportGetFingerprint(NULL, NULL), xErrno_Ok);

  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);
  EXPECT_NE(xDtlsTransportGetFingerprint(t, NULL), xErrno_Ok);
  xDtlsTransportDestroy(t);
}

/* ═══════════════════════════════════════════════════════════
 *  NULL Safety for Query APIs
 * ═══════════════════════════════════════════════════════════ */

TEST(DtlsNullSafety, GetStateNull) {
  EXPECT_EQ(xDtlsTransportGetState(NULL), xDtlsState_Closed);
}

TEST(DtlsNullSafety, GetRoleNull) {
  EXPECT_EQ(xDtlsTransportGetRole(NULL), xDtlsRole_Passive);
}

TEST(DtlsNullSafety, StartNull) {
  EXPECT_NE(xDtlsTransportStart(NULL), xErrno_Ok);
}

TEST(DtlsNullSafety, FeedInputNull) {
  EXPECT_NE(xDtlsTransportFeedInput(NULL, NULL, 0), xErrno_Ok);
}

TEST(DtlsNullSafety, SendNull) {
  EXPECT_NE(xDtlsTransportSend(NULL, NULL, 0), xErrno_Ok);
}

TEST(DtlsNullSafety, GetFingerprintStrNull) {
  EXPECT_NE(xDtlsTransportGetFingerprintStr(NULL, NULL), xErrno_Ok);
}

/* ═══════════════════════════════════════════════════════════
 *  Start Without Handshake Partner (state transitions)
 * ═══════════════════════════════════════════════════════════ */

class DtlsStartTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
  }
  void TearDown() override {
    if (loop) xEventLoopDestroy(loop);
  }
};

TEST_F(DtlsStartTest, StartActiveTransitionsToConnecting) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  xErrno err = xDtlsTransportStart(t);
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(xDtlsTransportGetState(t), xDtlsState_Connecting);

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsStartTest, StartPassiveTransitionsToConnecting) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Passive;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  xErrno err = xDtlsTransportStart(t);
  EXPECT_EQ(err, xErrno_Ok);
  EXPECT_EQ(xDtlsTransportGetState(t), xDtlsState_Connecting);

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsStartTest, DoubleStartFails) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_EQ(xDtlsTransportStart(t), xErrno_Ok);
  EXPECT_NE(xDtlsTransportStart(t), xErrno_Ok); /* Already started */

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsStartTest, SendBeforeHandshakeFails) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  uint8_t data[] = {1, 2, 3};
  /* Send before start should fail */
  EXPECT_NE(xDtlsTransportSend(t, data, sizeof(data)), xErrno_Ok);

  /* Send during connecting should also fail */
  xDtlsTransportStart(t);
  EXPECT_NE(xDtlsTransportSend(t, data, sizeof(data)), xErrno_Ok);

  xDtlsTransportDestroy(t);
}

/* ═══════════════════════════════════════════════════════════
 *  Active ↔ Passive Handshake (loopback)
 * ═══════════════════════════════════════════════════════════ */

/**
 * Test fixture that creates an Active and a Passive DTLS transport
 * wired together via in-memory send callbacks. This simulates a
 * real DTLS handshake without any network.
 */
class DtlsHandshakeTest : public ::testing::Test {
protected:
  xEventLoop     loop    = nullptr;
  xDtlsTransport active  = nullptr;
  xDtlsTransport passive = nullptr;

  /* State tracking */
  xDtlsState active_state  = xDtlsState_New;
  xDtlsState passive_state = xDtlsState_New;

  /* Received application data */
  std::vector<uint8_t> active_received;
  std::vector<uint8_t> passive_received;

  void SetUp() override {
    loop = xEventLoopCreate();
    ASSERT_NE(loop, nullptr);
  }

  void TearDown() override {
    if (active) xDtlsTransportDestroy(active);
    if (passive) xDtlsTransportDestroy(passive);
    if (loop) xEventLoopDestroy(loop);
  }

  /* Create the Active ↔ Passive pair without fingerprint verification */
  void CreatePair(bool verify_fingerprint = false) {
    /* Create passive first so active can reference it */
    xDtlsTransportConf passive_conf;
    memset(&passive_conf, 0, sizeof(passive_conf));
    passive_conf.loop            = loop;
    passive_conf.role            = xDtlsRole_Passive;
    passive_conf.send_fn         = passive_send_cb;
    passive_conf.send_arg        = this;
    passive_conf.on_state_change = passive_state_cb;
    passive_conf.on_data         = passive_data_cb;
    passive_conf.ctx             = this;

    passive = xDtlsTransportCreate(&passive_conf);
    ASSERT_NE(passive, nullptr);

    /* Get passive fingerprint for active to verify */
    uint8_t passive_fp[XDTLS_FINGERPRINT_SIZE];
    memset(passive_fp, 0, sizeof(passive_fp));
    if (verify_fingerprint) {
      ASSERT_EQ(xDtlsTransportGetFingerprint(passive, passive_fp), xErrno_Ok);
    }

    xDtlsTransportConf active_conf;
    memset(&active_conf, 0, sizeof(active_conf));
    active_conf.loop               = loop;
    active_conf.role               = xDtlsRole_Active;
    active_conf.send_fn            = active_send_cb;
    active_conf.send_arg           = this;
    active_conf.on_state_change    = active_state_cb;
    active_conf.on_data            = active_data_cb;
    active_conf.ctx                = this;
    active_conf.verify_fingerprint = verify_fingerprint;
    if (verify_fingerprint) {
      memcpy(active_conf.remote_fingerprint, passive_fp, XDTLS_FINGERPRINT_SIZE);
    }

    active = xDtlsTransportCreate(&active_conf);
    ASSERT_NE(active, nullptr);
  }

  /* Run the handshake loop until both sides are connected or timeout */
  bool RunHandshake(int timeout_ms = 3000) {
    int       elapsed = 0;
    const int step    = 5;
    while (elapsed < timeout_ms) {
      xEventWait(loop, step);
      elapsed += step;

      if (active_state == xDtlsState_Connected && passive_state == xDtlsState_Connected) {
        return true;
      }
      if (active_state == xDtlsState_Failed || passive_state == xDtlsState_Failed) {
        return false;
      }
    }
    return false;
  }

private:
  /* ── Send callbacks: route encrypted data to the other side ── */

  static xErrno active_send_cb(const uint8_t *data, size_t len, void *arg) {
    auto *self = static_cast<DtlsHandshakeTest *>(arg);
    /* Active's output → feed into Passive's input */
    if (self->passive) {
      xDtlsTransportFeedInput(self->passive, data, len);
    }
    return xErrno_Ok;
  }

  static xErrno passive_send_cb(const uint8_t *data, size_t len, void *arg) {
    auto *self = static_cast<DtlsHandshakeTest *>(arg);
    /* Passive's output → feed into Active's input */
    if (self->active) {
      xDtlsTransportFeedInput(self->active, data, len);
    }
    return xErrno_Ok;
  }

  /* ── State change callbacks ── */

  static void active_state_cb(xDtlsTransport /*t*/, xDtlsState state, void *arg) {
    auto *self         = static_cast<DtlsHandshakeTest *>(arg);
    self->active_state = state;
  }

  static void passive_state_cb(xDtlsTransport /*t*/, xDtlsState state, void *arg) {
    auto *self          = static_cast<DtlsHandshakeTest *>(arg);
    self->passive_state = state;
  }

  /* ── Data callbacks ── */

  static void active_data_cb(xDtlsTransport /*t*/, const uint8_t *data, size_t len, void *arg) {
    auto *self = static_cast<DtlsHandshakeTest *>(arg);
    self->active_received.insert(self->active_received.end(), data, data + len);
  }

  static void passive_data_cb(xDtlsTransport /*t*/, const uint8_t *data, size_t len, void *arg) {
    auto *self = static_cast<DtlsHandshakeTest *>(arg);
    self->passive_received.insert(self->passive_received.end(), data, data + len);
  }
};

TEST_F(DtlsHandshakeTest, ActivePassiveHandshakeCompletes) {
  CreatePair(false);

  /* Start both sides */
  ASSERT_EQ(xDtlsTransportStart(active), xErrno_Ok);
  ASSERT_EQ(xDtlsTransportStart(passive), xErrno_Ok);

  /* Run handshake */
  ASSERT_TRUE(RunHandshake()) << "Handshake did not complete within timeout. "
                              << "Active state: " << active_state
                              << ", Passive state: " << passive_state;

  EXPECT_EQ(xDtlsTransportGetState(active), xDtlsState_Connected);
  EXPECT_EQ(xDtlsTransportGetState(passive), xDtlsState_Connected);
}

TEST_F(DtlsHandshakeTest, HandshakeWithFingerprintVerification) {
  CreatePair(true /* verify_fingerprint */);

  ASSERT_EQ(xDtlsTransportStart(active), xErrno_Ok);
  ASSERT_EQ(xDtlsTransportStart(passive), xErrno_Ok);

  ASSERT_TRUE(RunHandshake()) << "Handshake with fingerprint verification failed. "
                              << "Active state: " << active_state
                              << ", Passive state: " << passive_state;

  EXPECT_EQ(xDtlsTransportGetState(active), xDtlsState_Connected);
  EXPECT_EQ(xDtlsTransportGetState(passive), xDtlsState_Connected);
}

TEST_F(DtlsHandshakeTest, DataExchangeAfterHandshake) {
  CreatePair(false);

  ASSERT_EQ(xDtlsTransportStart(active), xErrno_Ok);
  ASSERT_EQ(xDtlsTransportStart(passive), xErrno_Ok);
  ASSERT_TRUE(RunHandshake());

  /* Send data from Active → Passive */
  const char *msg1 = "Hello from Active!";
  xErrno      err  = xDtlsTransportSend(active, (const uint8_t *)msg1, strlen(msg1));
  ASSERT_EQ(err, xErrno_Ok);

  /* Tick to let data flow */
  tick_loop(loop, 50);

  ASSERT_EQ(passive_received.size(), strlen(msg1));
  EXPECT_EQ(memcmp(passive_received.data(), msg1, strlen(msg1)), 0);

  /* Send data from Passive → Active */
  const char *msg2 = "Hello from Passive!";
  err              = xDtlsTransportSend(passive, (const uint8_t *)msg2, strlen(msg2));
  ASSERT_EQ(err, xErrno_Ok);

  tick_loop(loop, 50);

  ASSERT_EQ(active_received.size(), strlen(msg2));
  EXPECT_EQ(memcmp(active_received.data(), msg2, strlen(msg2)), 0);
}

TEST_F(DtlsHandshakeTest, LargeDataExchange) {
  CreatePair(false);

  ASSERT_EQ(xDtlsTransportStart(active), xErrno_Ok);
  ASSERT_EQ(xDtlsTransportStart(passive), xErrno_Ok);
  ASSERT_TRUE(RunHandshake());

  /* Send a larger payload (4000 bytes) */
  std::vector<uint8_t> payload(4000);
  for (size_t i = 0; i < payload.size(); i++) {
    payload[i] = (uint8_t)(i & 0xFF);
  }

  xErrno err = xDtlsTransportSend(active, payload.data(), payload.size());
  ASSERT_EQ(err, xErrno_Ok);

  tick_loop(loop, 100);

  ASSERT_EQ(passive_received.size(), payload.size());
  EXPECT_EQ(memcmp(passive_received.data(), payload.data(), payload.size()), 0);
}

/* ═══════════════════════════════════════════════════════════
 *  Wrong Fingerprint Verification
 * ═══════════════════════════════════════════════════════════ */

class DtlsWrongFingerprintTest : public ::testing::Test {
protected:
  xEventLoop     loop    = nullptr;
  xDtlsTransport active  = nullptr;
  xDtlsTransport passive = nullptr;

  xDtlsState active_state  = xDtlsState_New;
  xDtlsState passive_state = xDtlsState_New;

  void SetUp() override {
    loop = xEventLoopCreate();
  }
  void TearDown() override {
    if (active) xDtlsTransportDestroy(active);
    if (passive) xDtlsTransportDestroy(passive);
    if (loop) xEventLoopDestroy(loop);
  }

  static xErrno active_send(const uint8_t *data, size_t len, void *arg) {
    auto *self = static_cast<DtlsWrongFingerprintTest *>(arg);
    if (self->passive) xDtlsTransportFeedInput(self->passive, data, len);
    return xErrno_Ok;
  }

  static xErrno passive_send(const uint8_t *data, size_t len, void *arg) {
    auto *self = static_cast<DtlsWrongFingerprintTest *>(arg);
    if (self->active) xDtlsTransportFeedInput(self->active, data, len);
    return xErrno_Ok;
  }

  static void active_state_cb(xDtlsTransport, xDtlsState s, void *arg) {
    static_cast<DtlsWrongFingerprintTest *>(arg)->active_state = s;
  }

  static void passive_state_cb(xDtlsTransport, xDtlsState s, void *arg) {
    static_cast<DtlsWrongFingerprintTest *>(arg)->passive_state = s;
  }
};

TEST_F(DtlsWrongFingerprintTest, WrongFingerprintCausesFailure) {
  /* Create passive */
  xDtlsTransportConf passive_conf;
  memset(&passive_conf, 0, sizeof(passive_conf));
  passive_conf.loop            = loop;
  passive_conf.role            = xDtlsRole_Passive;
  passive_conf.send_fn         = passive_send;
  passive_conf.send_arg        = this;
  passive_conf.on_state_change = passive_state_cb;
  passive_conf.ctx             = this;

  passive = xDtlsTransportCreate(&passive_conf);
  ASSERT_NE(passive, nullptr);

  /* Create active with WRONG fingerprint */
  uint8_t wrong_fp[XDTLS_FINGERPRINT_SIZE];
  memset(wrong_fp, 0xDE, sizeof(wrong_fp)); /* Definitely wrong */

  xDtlsTransportConf active_conf;
  memset(&active_conf, 0, sizeof(active_conf));
  active_conf.loop               = loop;
  active_conf.role               = xDtlsRole_Active;
  active_conf.send_fn            = active_send;
  active_conf.send_arg           = this;
  active_conf.on_state_change    = active_state_cb;
  active_conf.ctx                = this;
  active_conf.verify_fingerprint = true;
  memcpy(active_conf.remote_fingerprint, wrong_fp, XDTLS_FINGERPRINT_SIZE);

  active = xDtlsTransportCreate(&active_conf);
  ASSERT_NE(active, nullptr);

  ASSERT_EQ(xDtlsTransportStart(active), xErrno_Ok);
  ASSERT_EQ(xDtlsTransportStart(passive), xErrno_Ok);

  /* Run handshake — should fail due to fingerprint mismatch */
  int  elapsed = 0;
  bool failed  = false;
  while (elapsed < 5000) {
    xEventWait(loop, 10);
    elapsed += 10;
    if (active_state == xDtlsState_Failed) {
      failed = true;
      break;
    }
  }

  EXPECT_TRUE(failed) << "Expected handshake to fail due to wrong fingerprint, "
                      << "but active state is: " << active_state;
}

/* ═══════════════════════════════════════════════════════════
 *  Handshake Timeout
 * ═══════════════════════════════════════════════════════════ */

class DtlsTimeoutTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
  }
  void TearDown() override {
    if (loop) xEventLoopDestroy(loop);
  }
};

TEST_F(DtlsTimeoutTest, PassiveTimesOutWithoutPeer) {
  xDtlsState last_state = xDtlsState_New;

  auto state_cb = [](xDtlsTransport, xDtlsState s, void *arg) {
    *static_cast<xDtlsState *>(arg) = s;
  };

  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop                 = loop;
  conf.role                 = xDtlsRole_Passive;
  conf.send_fn              = dummy_send;
  conf.on_state_change      = state_cb;
  conf.ctx                  = &last_state;
  conf.handshake_timeout_ms = 200; /* Short timeout for test */

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  ASSERT_EQ(xDtlsTransportStart(t), xErrno_Ok);
  EXPECT_EQ(last_state, xDtlsState_Connecting);

  /* Tick past the timeout */
  tick_loop(loop, 500);

  EXPECT_EQ(last_state, xDtlsState_Failed)
    << "Expected timeout failure, but state is: " << last_state;

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsTimeoutTest, ActiveTimesOutWithoutPeer) {
  xDtlsState last_state = xDtlsState_New;

  auto state_cb = [](xDtlsTransport, xDtlsState s, void *arg) {
    *static_cast<xDtlsState *>(arg) = s;
  };

  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop                 = loop;
  conf.role                 = xDtlsRole_Active;
  conf.send_fn              = dummy_send;
  conf.on_state_change      = state_cb;
  conf.ctx                  = &last_state;
  conf.handshake_timeout_ms = 200;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  ASSERT_EQ(xDtlsTransportStart(t), xErrno_Ok);
  EXPECT_EQ(last_state, xDtlsState_Connecting);

  tick_loop(loop, 500);

  EXPECT_EQ(last_state, xDtlsState_Failed)
    << "Expected timeout failure, but state is: " << last_state;

  xDtlsTransportDestroy(t);
}

/* ═══════════════════════════════════════════════════════════
 *  Role Consistency Tests
 * ═══════════════════════════════════════════════════════════ */

class DtlsRoleTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
  }
  void TearDown() override {
    if (loop) xEventLoopDestroy(loop);
  }
};

TEST_F(DtlsRoleTest, ActiveRoleIsActive) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(xDtlsTransportGetRole(t), xDtlsRole_Active);
  xDtlsTransportDestroy(t);
}

TEST_F(DtlsRoleTest, PassiveRoleIsPassive) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Passive;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(xDtlsTransportGetRole(t), xDtlsRole_Passive);
  xDtlsTransportDestroy(t);
}

TEST_F(DtlsRoleTest, ActpassResolvesToPassive) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Actpass;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(xDtlsTransportGetRole(t), xDtlsRole_Passive);
  xDtlsTransportDestroy(t);
}

/* ═══════════════════════════════════════════════════════════
 *  FeedInput Edge Cases
 * ═══════════════════════════════════════════════════════════ */

class DtlsFeedInputTest : public ::testing::Test {
protected:
  xEventLoop loop = nullptr;

  void SetUp() override {
    loop = xEventLoopCreate();
  }
  void TearDown() override {
    if (loop) xEventLoopDestroy(loop);
  }
};

TEST_F(DtlsFeedInputTest, FeedInputNullData) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  EXPECT_NE(xDtlsTransportFeedInput(t, NULL, 0), xErrno_Ok);

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsFeedInputTest, FeedInputZeroLen) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Active;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  uint8_t data[] = {1};
  EXPECT_NE(xDtlsTransportFeedInput(t, data, 0), xErrno_Ok);

  xDtlsTransportDestroy(t);
}

TEST_F(DtlsFeedInputTest, FeedGarbageDoesNotCrash) {
  xDtlsTransportConf conf;
  memset(&conf, 0, sizeof(conf));
  conf.loop    = loop;
  conf.role    = xDtlsRole_Passive;
  conf.send_fn = dummy_send;

  xDtlsTransport t = xDtlsTransportCreate(&conf);
  ASSERT_NE(t, nullptr);

  xDtlsTransportStart(t);

  /* Feed random garbage — should not crash */
  uint8_t garbage[64];
  memset(garbage, 0xAB, sizeof(garbage));
  xDtlsTransportFeedInput(t, garbage, sizeof(garbage));

  /* Tick to process */
  tick_loop(loop, 50);

  /* Transport should still be alive (connecting or failed, not crashed) */
  xDtlsState state = xDtlsTransportGetState(t);
  EXPECT_TRUE(state == xDtlsState_Connecting || state == xDtlsState_Failed);

  xDtlsTransportDestroy(t);
}
