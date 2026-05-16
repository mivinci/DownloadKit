/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sha1_test.cpp - Unit tests for xSha1
 */

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

/* ── RFC 3174 test vectors ─────────────────────────────── */

TEST(Sha1, EmptyString) {
  uint8_t digest[XCRYPTO_SHA1_DIGEST_SIZE];
  ASSERT_EQ(xSha1((const uint8_t *)"", 0, digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA1_DIGEST_SIZE), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(Sha1, Abc) {
  const char *input = "abc";
  uint8_t     digest[XCRYPTO_SHA1_DIGEST_SIZE];
  ASSERT_EQ(xSha1((const uint8_t *)input, strlen(input), digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA1_DIGEST_SIZE), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(Sha1, AbcLong) {
  const char *input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  uint8_t     digest[XCRYPTO_SHA1_DIGEST_SIZE];
  ASSERT_EQ(xSha1((const uint8_t *)input, strlen(input), digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA1_DIGEST_SIZE), "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(Sha1, MillionAs) {
  /* 1,000,000 repetitions of 'a' */
  std::string input(1000000, 'a');
  uint8_t     digest[XCRYPTO_SHA1_DIGEST_SIZE];
  ASSERT_EQ(xSha1((const uint8_t *)input.data(), input.size(), digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA1_DIGEST_SIZE), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

/* ── Streaming API ─────────────────────────────────────── */

TEST(Sha1, StreamingMatchesOneShot) {
  const char *parts[] = {"Hello, ", "World!"};
  const char *full    = "Hello, World!";

  /* One-shot */
  uint8_t digest1[XCRYPTO_SHA1_DIGEST_SIZE];
  ASSERT_EQ(xSha1((const uint8_t *)full, strlen(full), digest1), xErrno_Ok);

  /* Streaming */
  uint8_t  digest2[XCRYPTO_SHA1_DIGEST_SIZE];
  xSha1Ctx ctx;
  ASSERT_EQ(xSha1Init(&ctx), xErrno_Ok);
  ASSERT_EQ(xSha1Update(&ctx, (const uint8_t *)parts[0], strlen(parts[0])), xErrno_Ok);
  ASSERT_EQ(xSha1Update(&ctx, (const uint8_t *)parts[1], strlen(parts[1])), xErrno_Ok);
  ASSERT_EQ(xSha1Final(&ctx, digest2), xErrno_Ok);

  EXPECT_EQ(memcmp(digest1, digest2, XCRYPTO_SHA1_DIGEST_SIZE), 0);
}

/* ── Error handling ────────────────────────────────────── */

TEST(Sha1, NullArgs) {
  uint8_t digest[XCRYPTO_SHA1_DIGEST_SIZE];
  EXPECT_EQ(xSha1(NULL, 0, digest), xErrno_InvalidArg);
  EXPECT_EQ(xSha1((const uint8_t *)"x", 1, NULL), xErrno_InvalidArg);
  EXPECT_EQ(xSha1Init(NULL), xErrno_InvalidArg);
}
