/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hmac_sha1_test.cpp - Unit tests for HMAC-SHA1
 */

#include <x/crypto/hmac.h>
#include <x/crypto/hmac_sha1.h>
#include <x/crypto/sha1.h>

#include <gtest/gtest.h>

#include <cstring>
#include <string>

/* ── Helper: hex-encode a digest ───────────────────────── */

static std::string hex(const uint8_t *data, size_t len) {
  static const char digits[] = "0123456789abcdef";
  std::string       result;
  result.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    result.push_back(digits[data[i] >> 4]);
    result.push_back(digits[data[i] & 0x0f]);
  }
  return result;
}

/* ═══════════════════════════════════════════════════════════════════
 *  HMAC-SHA1 (RFC 2202 Test Vectors)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(HmacSha1, Rfc2202_1) {
  /* Test Case 1: key = 0x0b * 20, data = "Hi There" */
  uint8_t key[20];
  memset(key, 0x0b, sizeof(key));
  const char *data = "Hi There";
  uint8_t     digest[XCRYPTO_SHA1_DIGEST_SIZE];

  ASSERT_EQ(xHmacSha1(key, sizeof(key), (const uint8_t *)data, strlen(data), digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA1_DIGEST_SIZE), "b617318655057264e28bc0b6fb378c8ef146be00");
}

TEST(HmacSha1, Rfc2202_2) {
  /* Test Case 2: key = "Jefe", data = "what do ya want for nothing?" */
  const char *key  = "Jefe";
  const char *data = "what do ya want for nothing?";
  uint8_t     digest[XCRYPTO_SHA1_DIGEST_SIZE];

  ASSERT_EQ(
    xHmacSha1((const uint8_t *)key, strlen(key), (const uint8_t *)data, strlen(data), digest),
    xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA1_DIGEST_SIZE), "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79");
}

TEST(HmacSha1, Rfc2202_3) {
  /* Test Case 3: key = 0xaa * 20, data = 0xdd * 50 */
  uint8_t key[20];
  memset(key, 0xaa, sizeof(key));
  uint8_t data[50];
  memset(data, 0xdd, sizeof(data));
  uint8_t digest[XCRYPTO_SHA1_DIGEST_SIZE];

  ASSERT_EQ(xHmacSha1(key, sizeof(key), data, sizeof(data), digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA1_DIGEST_SIZE), "125d7342b9ac11cd91a39af48aa17b4f63f175d3");
}

TEST(HmacSha1, NullArgs) {
  uint8_t digest[XCRYPTO_SHA1_DIGEST_SIZE];
  EXPECT_EQ(xHmacSha1(NULL, 4, (const uint8_t *)"x", 1, digest), xErrno_InvalidArg);
  EXPECT_EQ(xHmacSha1((const uint8_t *)"k", 1, NULL, 0, digest), xErrno_InvalidArg);
  EXPECT_EQ(xHmacSha1((const uint8_t *)"k", 1, (const uint8_t *)"x", 1, NULL), xErrno_InvalidArg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic xHmac with SHA-1 vtable
 * ═══════════════════════════════════════════════════════════════════ */

TEST(HmacSha1, GenericMatchesWrapper) {
  /* Verify xHmac(&xHashVtableSha1, ...) == xHmacSha1(...) */
  const char *key  = "secret";
  const char *data = "hello world";
  uint8_t     d1[XCRYPTO_SHA1_DIGEST_SIZE];
  uint8_t     d2[XCRYPTO_SHA1_DIGEST_SIZE];

  ASSERT_EQ(xHmacSha1((const uint8_t *)key, strlen(key), (const uint8_t *)data, strlen(data), d1),
            xErrno_Ok);
  ASSERT_EQ(xHmac(&xHashVtableSha1, (const uint8_t *)key, strlen(key), (const uint8_t *)data,
                  strlen(data), d2),
            xErrno_Ok);
  EXPECT_EQ(memcmp(d1, d2, XCRYPTO_SHA1_DIGEST_SIZE), 0);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic xHmac null check
 * ═══════════════════════════════════════════════════════════════════ */

TEST(Hmac, NullHash) {
  uint8_t digest[20];
  EXPECT_EQ(xHmac(NULL, (const uint8_t *)"k", 1, (const uint8_t *)"d", 1, digest),
            xErrno_InvalidArg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Streaming HMAC-SHA1
 * ═══════════════════════════════════════════════════════════════════ */

TEST(HmacSha1Streaming, MatchesOneShot) {
  /* Streaming result must match one-shot xHmacSha1 */
  const char *key  = "Jefe";
  const char *data = "what do ya want for nothing?";
  uint8_t     d_oneshot[XCRYPTO_SHA1_DIGEST_SIZE];
  uint8_t     d_stream[XCRYPTO_SHA1_DIGEST_SIZE];

  ASSERT_EQ(
    xHmacSha1((const uint8_t *)key, strlen(key), (const uint8_t *)data, strlen(data), d_oneshot),
    xErrno_Ok);

  xHmacCtx ctx;
  ASSERT_EQ(xHmacInit(&ctx, &xHashVtableSha1, (const uint8_t *)key, strlen(key)), xErrno_Ok);
  ASSERT_EQ(xHmacUpdate(&ctx, (const uint8_t *)data, strlen(data)), xErrno_Ok);
  ASSERT_EQ(xHmacFinal(&ctx, d_stream), xErrno_Ok);

  EXPECT_EQ(memcmp(d_oneshot, d_stream, XCRYPTO_SHA1_DIGEST_SIZE), 0);
}

TEST(HmacSha1Streaming, MultipleUpdates) {
  /* Feed data in chunks — must produce the same digest */
  uint8_t key[20];
  memset(key, 0x0b, sizeof(key));
  const char *data = "Hi There";
  uint8_t     d_oneshot[XCRYPTO_SHA1_DIGEST_SIZE];
  uint8_t     d_stream[XCRYPTO_SHA1_DIGEST_SIZE];

  ASSERT_EQ(xHmacSha1(key, sizeof(key), (const uint8_t *)data, strlen(data), d_oneshot), xErrno_Ok);

  xHmacCtx ctx;
  ASSERT_EQ(xHmacInit(&ctx, &xHashVtableSha1, key, sizeof(key)), xErrno_Ok);
  /* Feed one byte at a time */
  for (size_t i = 0; i < strlen(data); i++) {
    ASSERT_EQ(xHmacUpdate(&ctx, (const uint8_t *)&data[i], 1), xErrno_Ok);
  }
  ASSERT_EQ(xHmacFinal(&ctx, d_stream), xErrno_Ok);

  EXPECT_EQ(memcmp(d_oneshot, d_stream, XCRYPTO_SHA1_DIGEST_SIZE), 0);
}

TEST(HmacStreaming, NullArgs) {
  xHmacCtx ctx;
  uint8_t  digest[20];

  EXPECT_EQ(xHmacInit(NULL, &xHashVtableSha1, (const uint8_t *)"k", 1), xErrno_InvalidArg);
  EXPECT_EQ(xHmacInit(&ctx, NULL, (const uint8_t *)"k", 1), xErrno_InvalidArg);
  EXPECT_EQ(xHmacInit(&ctx, &xHashVtableSha1, NULL, 1), xErrno_InvalidArg);

  EXPECT_EQ(xHmacUpdate(NULL, (const uint8_t *)"d", 1), xErrno_InvalidArg);
  EXPECT_EQ(xHmacFinal(NULL, digest), xErrno_InvalidArg);
}
