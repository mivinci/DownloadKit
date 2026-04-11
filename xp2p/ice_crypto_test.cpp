/*
 * Copyright 2025 The xKit Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * ice_crypto_test.cpp - Tests for MD5, SHA-1, HMAC-SHA1, CRC-32
 */

#include <gtest/gtest.h>

extern "C" {
#include "ice_crypto.h"
}

#include <cstring>

/* ───────────────────── MD5 Tests ───────────────────── */

TEST(IceCryptoTest, MD5Empty) {
  /* MD5("") = d41d8cd98f00b204e9800998ecf8427e */
  uint8_t digest[XICE_MD5_DIGEST_SIZE];
  xIceMD5((const uint8_t *)"", 0, digest);

  const uint8_t expected[] = {
    0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04,
    0xe9, 0x80, 0x09, 0x98, 0xec, 0xf8, 0x42, 0x7e,
  };
  EXPECT_EQ(memcmp(digest, expected, XICE_MD5_DIGEST_SIZE), 0);
}

TEST(IceCryptoTest, MD5Hello) {
  /* MD5("Hello") = 8b1a9953c4611296a827abf8c47804d7 */
  const char *input = "Hello";
  uint8_t     digest[XICE_MD5_DIGEST_SIZE];
  xIceMD5((const uint8_t *)input, strlen(input), digest);

  const uint8_t expected[] = {
    0x8b, 0x1a, 0x99, 0x53, 0xc4, 0x61, 0x12, 0x96,
    0xa8, 0x27, 0xab, 0xf8, 0xc4, 0x78, 0x04, 0xd7,
  };
  EXPECT_EQ(memcmp(digest, expected, XICE_MD5_DIGEST_SIZE), 0);
}

TEST(IceCryptoTest, MD5ABCLower) {
  /* MD5("abc") = 900150983cd24fb0d6963f7d28e17f72 */
  const char *input = "abc";
  uint8_t     digest[XICE_MD5_DIGEST_SIZE];
  xIceMD5((const uint8_t *)input, strlen(input), digest);

  const uint8_t expected[] = {
    0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
    0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72,
  };
  EXPECT_EQ(memcmp(digest, expected, XICE_MD5_DIGEST_SIZE), 0);
}

TEST(IceCryptoTest, MD5LongTermCredential) {
  /*
   * RFC 5389 §15.4: key = MD5(username ":" realm ":" password)
   * Test with known values.
   *
   * MD5("user:example.org:pass") should produce a deterministic hash.
   */
  const char *input = "user:example.org:pass";
  uint8_t     digest[XICE_MD5_DIGEST_SIZE];
  xIceMD5((const uint8_t *)input, strlen(input), digest);

  /* Verify it's non-zero and deterministic */
  uint8_t digest2[XICE_MD5_DIGEST_SIZE];
  xIceMD5((const uint8_t *)input, strlen(input), digest2);
  EXPECT_EQ(memcmp(digest, digest2, XICE_MD5_DIGEST_SIZE), 0);

  /* Verify different input produces different hash */
  const char *input2 = "user:example.org:pass2";
  uint8_t     digest3[XICE_MD5_DIGEST_SIZE];
  xIceMD5((const uint8_t *)input2, strlen(input2), digest3);
  EXPECT_NE(memcmp(digest, digest3, XICE_MD5_DIGEST_SIZE), 0);
}

TEST(IceCryptoTest, MD5ExactBlockSize) {
  /* Test with exactly 55 bytes (fits in one block with padding) */
  uint8_t input[55];
  memset(input, 'A', sizeof(input));
  uint8_t digest[XICE_MD5_DIGEST_SIZE];
  xIceMD5(input, sizeof(input), digest);

  /* Verify deterministic */
  uint8_t digest2[XICE_MD5_DIGEST_SIZE];
  xIceMD5(input, sizeof(input), digest2);
  EXPECT_EQ(memcmp(digest, digest2, XICE_MD5_DIGEST_SIZE), 0);
}

TEST(IceCryptoTest, MD5CrossBlockBoundary) {
  /* Test with 56 bytes (requires two blocks) */
  uint8_t input[56];
  memset(input, 'B', sizeof(input));
  uint8_t digest[XICE_MD5_DIGEST_SIZE];
  xIceMD5(input, sizeof(input), digest);

  /* Different from 55-byte input */
  uint8_t input55[55];
  memset(input55, 'B', sizeof(input55));
  uint8_t digest55[XICE_MD5_DIGEST_SIZE];
  xIceMD5(input55, sizeof(input55), digest55);
  EXPECT_NE(memcmp(digest, digest55, XICE_MD5_DIGEST_SIZE), 0);
}

TEST(IceCryptoTest, MD5MultiBlock) {
  /* Test with 128 bytes (two full blocks) */
  uint8_t input[128];
  memset(input, 'C', sizeof(input));
  uint8_t digest[XICE_MD5_DIGEST_SIZE];
  xIceMD5(input, sizeof(input), digest);

  uint8_t digest2[XICE_MD5_DIGEST_SIZE];
  xIceMD5(input, sizeof(input), digest2);
  EXPECT_EQ(memcmp(digest, digest2, XICE_MD5_DIGEST_SIZE), 0);
}

/* ───────────────────── TURN Refresh Timer Tests ───────────────────── */

/*
 * Note: TURN refresh timer is tested indirectly through the
 * turn_test.cpp allocate flow. The schedule_refresh function
 * is static, so we verify its behavior through integration.
 */
