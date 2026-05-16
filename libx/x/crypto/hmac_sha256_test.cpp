/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hmac_sha256_test.cpp - Unit tests for HMAC-SHA256
 */

#include <x/crypto/hmac.h>
#include <x/crypto/hmac_sha256.h>
#include <x/crypto/sha256.h>

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
 *  HMAC-SHA256 (RFC 4231 Test Vectors)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(HmacSha256, Rfc4231_1) {
  /* Test Case 1: key = 0x0b * 20, data = "Hi There" */
  uint8_t key[20];
  memset(key, 0x0b, sizeof(key));
  const char *data = "Hi There";
  uint8_t     digest[XCRYPTO_SHA256_DIGEST_SIZE];

  ASSERT_EQ(xHmacSha256(key, sizeof(key), (const uint8_t *)data, strlen(data),
                         digest),
            xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA256_DIGEST_SIZE),
            "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(HmacSha256, Rfc4231_2) {
  /* Test Case 2: key = "Jefe", data = "what do ya want for nothing?" */
  const char *key  = "Jefe";
  const char *data = "what do ya want for nothing?";
  uint8_t     digest[XCRYPTO_SHA256_DIGEST_SIZE];

  ASSERT_EQ(xHmacSha256((const uint8_t *)key, strlen(key),
                         (const uint8_t *)data, strlen(data), digest),
            xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA256_DIGEST_SIZE),
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(HmacSha256, Rfc4231_3) {
  /* Test Case 3: key = 0xaa * 20, data = 0xdd * 50 */
  uint8_t key[20];
  memset(key, 0xaa, sizeof(key));
  uint8_t data[50];
  memset(data, 0xdd, sizeof(data));
  uint8_t digest[XCRYPTO_SHA256_DIGEST_SIZE];

  ASSERT_EQ(xHmacSha256(key, sizeof(key), data, sizeof(data), digest),
            xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA256_DIGEST_SIZE),
            "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
}

TEST(HmacSha256, NullArgs) {
  uint8_t digest[XCRYPTO_SHA256_DIGEST_SIZE];
  EXPECT_EQ(xHmacSha256(NULL, 4, (const uint8_t *)"x", 1, digest),
            xErrno_InvalidArg);
  EXPECT_EQ(xHmacSha256((const uint8_t *)"k", 1, NULL, 0, digest),
            xErrno_InvalidArg);
  EXPECT_EQ(xHmacSha256((const uint8_t *)"k", 1, (const uint8_t *)"x", 1, NULL),
            xErrno_InvalidArg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic xHmac with SHA-256 vtable
 * ═══════════════════════════════════════════════════════════════════ */

TEST(HmacSha256, GenericMatchesWrapper) {
  /* Verify xHmac(&xHashVtableSha256, ...) == xHmacSha256(...) */
  const char *key  = "secret";
  const char *data = "hello world";
  uint8_t     d1[XCRYPTO_SHA256_DIGEST_SIZE];
  uint8_t     d2[XCRYPTO_SHA256_DIGEST_SIZE];

  ASSERT_EQ(xHmacSha256((const uint8_t *)key, strlen(key),
                         (const uint8_t *)data, strlen(data), d1),
            xErrno_Ok);
  ASSERT_EQ(xHmac(&xHashVtableSha256, (const uint8_t *)key, strlen(key),
                   (const uint8_t *)data, strlen(data), d2),
            xErrno_Ok);
  EXPECT_EQ(memcmp(d1, d2, XCRYPTO_SHA256_DIGEST_SIZE), 0);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Streaming HMAC-SHA256
 * ═══════════════════════════════════════════════════════════════════ */

TEST(HmacSha256Streaming, MatchesOneShot) {
  /* Streaming result must match one-shot xHmacSha256 */
  const char *key  = "Jefe";
  const char *data = "what do ya want for nothing?";
  uint8_t     d_oneshot[XCRYPTO_SHA256_DIGEST_SIZE];
  uint8_t     d_stream[XCRYPTO_SHA256_DIGEST_SIZE];

  ASSERT_EQ(xHmacSha256((const uint8_t *)key, strlen(key),
                         (const uint8_t *)data, strlen(data), d_oneshot),
            xErrno_Ok);

  xHmacCtx ctx;
  ASSERT_EQ(xHmacInit(&ctx, &xHashVtableSha256,
                       (const uint8_t *)key, strlen(key)),
            xErrno_Ok);
  ASSERT_EQ(xHmacUpdate(&ctx, (const uint8_t *)data, strlen(data)),
            xErrno_Ok);
  ASSERT_EQ(xHmacFinal(&ctx, d_stream), xErrno_Ok);

  EXPECT_EQ(memcmp(d_oneshot, d_stream, XCRYPTO_SHA256_DIGEST_SIZE), 0);
}

TEST(HmacSha256Streaming, MultipleUpdates) {
  /* Feed data in chunks — must produce the same digest */
  uint8_t key[20];
  memset(key, 0x0b, sizeof(key));
  const char *data = "Hi There";
  uint8_t     d_oneshot[XCRYPTO_SHA256_DIGEST_SIZE];
  uint8_t     d_stream[XCRYPTO_SHA256_DIGEST_SIZE];

  ASSERT_EQ(xHmacSha256(key, sizeof(key), (const uint8_t *)data, strlen(data),
                         d_oneshot),
            xErrno_Ok);

  xHmacCtx ctx;
  ASSERT_EQ(xHmacInit(&ctx, &xHashVtableSha256, key, sizeof(key)), xErrno_Ok);
  /* Feed one byte at a time */
  for (size_t i = 0; i < strlen(data); i++) {
    ASSERT_EQ(xHmacUpdate(&ctx, (const uint8_t *)&data[i], 1), xErrno_Ok);
  }
  ASSERT_EQ(xHmacFinal(&ctx, d_stream), xErrno_Ok);

  EXPECT_EQ(memcmp(d_oneshot, d_stream, XCRYPTO_SHA256_DIGEST_SIZE), 0);
}
