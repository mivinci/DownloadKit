/*
 * Copyright 2025 The moo Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * md5_test.cpp - Unit tests for xMd5
 */

#include <cstring>
#include <gtest/gtest.h>
#include <x/crypto/md5.h>

/* ───────────────────── MD5 Tests ───────────────────── */

TEST(Md5, Empty) {
  uint8_t digest[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5((const uint8_t *)"", 0, digest), xErrno_Ok);

  const uint8_t expected[] = {
    0xd4, 0x1d, 0x8c, 0xd9, 0x8f, 0x00, 0xb2, 0x04,
    0xe9, 0x80, 0x09, 0x98, 0xec, 0xf8, 0x42, 0x7e,
  };
  EXPECT_EQ(memcmp(digest, expected, XCRYPTO_MD5_DIGEST_SIZE), 0);
}

TEST(Md5, Hello) {
  const char *input = "Hello";
  uint8_t     digest[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5((const uint8_t *)input, strlen(input), digest), xErrno_Ok);

  const uint8_t expected[] = {
    0x8b, 0x1a, 0x99, 0x53, 0xc4, 0x61, 0x12, 0x96,
    0xa8, 0x27, 0xab, 0xf8, 0xc4, 0x78, 0x04, 0xd7,
  };
  EXPECT_EQ(memcmp(digest, expected, XCRYPTO_MD5_DIGEST_SIZE), 0);
}

TEST(Md5, Abc) {
  const char *input = "abc";
  uint8_t     digest[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5((const uint8_t *)input, strlen(input), digest), xErrno_Ok);

  const uint8_t expected[] = {
    0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0,
    0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72,
  };
  EXPECT_EQ(memcmp(digest, expected, XCRYPTO_MD5_DIGEST_SIZE), 0);
}

TEST(Md5, LongTermCredential) {
  const char *input = "user:example.org:pass";
  uint8_t     digest[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5((const uint8_t *)input, strlen(input), digest), xErrno_Ok);

  /* Verify deterministic */
  uint8_t digest2[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5((const uint8_t *)input, strlen(input), digest2), xErrno_Ok);
  EXPECT_EQ(memcmp(digest, digest2, XCRYPTO_MD5_DIGEST_SIZE), 0);

  /* Verify different input produces different hash */
  const char *input2 = "user:example.org:pass2";
  uint8_t     digest3[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5((const uint8_t *)input2, strlen(input2), digest3), xErrno_Ok);
  EXPECT_NE(memcmp(digest, digest3, XCRYPTO_MD5_DIGEST_SIZE), 0);
}

TEST(Md5, ExactBlockSize) {
  uint8_t input[55];
  memset(input, 'A', sizeof(input));
  uint8_t digest[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5(input, sizeof(input), digest), xErrno_Ok);

  uint8_t digest2[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5(input, sizeof(input), digest2), xErrno_Ok);
  EXPECT_EQ(memcmp(digest, digest2, XCRYPTO_MD5_DIGEST_SIZE), 0);
}

TEST(Md5, CrossBlockBoundary) {
  uint8_t input[56];
  memset(input, 'B', sizeof(input));
  uint8_t digest[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5(input, sizeof(input), digest), xErrno_Ok);

  uint8_t input55[55];
  memset(input55, 'B', sizeof(input55));
  uint8_t digest55[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5(input55, sizeof(input55), digest55), xErrno_Ok);
  EXPECT_NE(memcmp(digest, digest55, XCRYPTO_MD5_DIGEST_SIZE), 0);
}

TEST(Md5, MultiBlock) {
  uint8_t input[128];
  memset(input, 'C', sizeof(input));
  uint8_t digest[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5(input, sizeof(input), digest), xErrno_Ok);

  uint8_t digest2[XCRYPTO_MD5_DIGEST_SIZE];
  ASSERT_EQ(xMd5(input, sizeof(input), digest2), xErrno_Ok);
  EXPECT_EQ(memcmp(digest, digest2, XCRYPTO_MD5_DIGEST_SIZE), 0);
}

TEST(Md5, NullArgs) {
  uint8_t digest[XCRYPTO_MD5_DIGEST_SIZE];
  EXPECT_EQ(xMd5(NULL, 0, digest), xErrno_InvalidArg);
  EXPECT_EQ(xMd5((const uint8_t *)"x", 1, NULL), xErrno_InvalidArg);
}
