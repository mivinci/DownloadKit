/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hmac_md5_test.cpp - Unit tests for HMAC-MD5
 */

#include <x/crypto/hmac.h>
#include <x/crypto/hmac_md5.h>
#include <x/crypto/md5.h>

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
 *  HMAC-MD5 (RFC 2202 Test Vectors)
 * ═══════════════════════════════════════════════════════════════════ */

TEST(HmacMd5, Rfc2202_1) {
  /* Test Case 1: key = 0x0b * 16, data = "Hi There" */
  uint8_t key[16];
  memset(key, 0x0b, sizeof(key));
  const char *data = "Hi There";
  uint8_t     digest[XCRYPTO_MD5_DIGEST_SIZE];

  ASSERT_EQ(xHmacMd5(key, sizeof(key), (const uint8_t *)data, strlen(data), digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_MD5_DIGEST_SIZE), "9294727a3638bb1c13f48ef8158bfc9d");
}

TEST(HmacMd5, Rfc2202_2) {
  /* Test Case 2: key = "Jefe", data = "what do ya want for nothing?" */
  const char *key  = "Jefe";
  const char *data = "what do ya want for nothing?";
  uint8_t     digest[XCRYPTO_MD5_DIGEST_SIZE];

  ASSERT_EQ(
    xHmacMd5((const uint8_t *)key, strlen(key), (const uint8_t *)data, strlen(data), digest),
    xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_MD5_DIGEST_SIZE), "750c783e6ab0b503eaa86e310a5db738");
}

TEST(HmacMd5, Rfc2202_3) {
  /* Test Case 3: key = 0xaa * 16, data = 0xdd * 50 */
  uint8_t key[16];
  memset(key, 0xaa, sizeof(key));
  uint8_t data[50];
  memset(data, 0xdd, sizeof(data));
  uint8_t digest[XCRYPTO_MD5_DIGEST_SIZE];

  ASSERT_EQ(xHmacMd5(key, sizeof(key), data, sizeof(data), digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_MD5_DIGEST_SIZE), "56be34521d144c88dbb8c733f0e8b3f6");
}

TEST(HmacMd5, NullArgs) {
  uint8_t digest[XCRYPTO_MD5_DIGEST_SIZE];
  EXPECT_EQ(xHmacMd5(NULL, 4, (const uint8_t *)"x", 1, digest), xErrno_InvalidArg);
  EXPECT_EQ(xHmacMd5((const uint8_t *)"k", 1, NULL, 0, digest), xErrno_InvalidArg);
  EXPECT_EQ(xHmacMd5((const uint8_t *)"k", 1, (const uint8_t *)"x", 1, NULL), xErrno_InvalidArg);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Generic xHmac with MD5 vtable
 * ═══════════════════════════════════════════════════════════════════ */

TEST(HmacMd5, GenericMatchesWrapper) {
  /* Verify xHmac(&xHashVtableMd5, ...) == xHmacMd5(...) */
  const char *key  = "secret";
  const char *data = "hello world";
  uint8_t     d1[XCRYPTO_MD5_DIGEST_SIZE];
  uint8_t     d2[XCRYPTO_MD5_DIGEST_SIZE];

  ASSERT_EQ(xHmacMd5((const uint8_t *)key, strlen(key), (const uint8_t *)data, strlen(data), d1),
            xErrno_Ok);
  ASSERT_EQ(xHmac(&xHashVtableMd5, (const uint8_t *)key, strlen(key), (const uint8_t *)data,
                  strlen(data), d2),
            xErrno_Ok);
  EXPECT_EQ(memcmp(d1, d2, XCRYPTO_MD5_DIGEST_SIZE), 0);
}

/* ═══════════════════════════════════════════════════════════════════
 *  Streaming HMAC-MD5
 * ═══════════════════════════════════════════════════════════════════ */

TEST(HmacMd5Streaming, MatchesOneShot) {
  /* Streaming result must match one-shot xHmacMd5 */
  const char *key  = "Jefe";
  const char *data = "what do ya want for nothing?";
  uint8_t     d_oneshot[XCRYPTO_MD5_DIGEST_SIZE];
  uint8_t     d_stream[XCRYPTO_MD5_DIGEST_SIZE];

  ASSERT_EQ(
    xHmacMd5((const uint8_t *)key, strlen(key), (const uint8_t *)data, strlen(data), d_oneshot),
    xErrno_Ok);

  xHmacCtx ctx;
  ASSERT_EQ(xHmacInit(&ctx, &xHashVtableMd5, (const uint8_t *)key, strlen(key)), xErrno_Ok);
  ASSERT_EQ(xHmacUpdate(&ctx, (const uint8_t *)data, strlen(data)), xErrno_Ok);
  ASSERT_EQ(xHmacFinal(&ctx, d_stream), xErrno_Ok);

  EXPECT_EQ(memcmp(d_oneshot, d_stream, XCRYPTO_MD5_DIGEST_SIZE), 0);
}

TEST(HmacMd5Streaming, MultipleUpdates) {
  /* Feed data in chunks — must produce the same digest */
  uint8_t key[16];
  memset(key, 0xaa, sizeof(key));
  uint8_t data[50];
  memset(data, 0xdd, sizeof(data));
  uint8_t d_oneshot[XCRYPTO_MD5_DIGEST_SIZE];
  uint8_t d_stream[XCRYPTO_MD5_DIGEST_SIZE];

  ASSERT_EQ(xHmacMd5(key, sizeof(key), data, sizeof(data), d_oneshot), xErrno_Ok);

  xHmacCtx ctx;
  ASSERT_EQ(xHmacInit(&ctx, &xHashVtableMd5, key, sizeof(key)), xErrno_Ok);
  /* Feed in two chunks: 20 + 30 */
  ASSERT_EQ(xHmacUpdate(&ctx, data, 20), xErrno_Ok);
  ASSERT_EQ(xHmacUpdate(&ctx, data + 20, 30), xErrno_Ok);
  ASSERT_EQ(xHmacFinal(&ctx, d_stream), xErrno_Ok);

  EXPECT_EQ(memcmp(d_oneshot, d_stream, XCRYPTO_MD5_DIGEST_SIZE), 0);
}
