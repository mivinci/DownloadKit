/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * sha256_test.cpp - Unit tests for xSha256
 */

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

/* ── NIST FIPS 180-4 test vectors ──────────────────────── */

TEST(Sha256, EmptyString) {
  uint8_t digest[XCRYPTO_SHA256_DIGEST_SIZE];
  ASSERT_EQ(xSha256((const uint8_t *)"", 0, digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA256_DIGEST_SIZE),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, Abc) {
  const char *input = "abc";
  uint8_t     digest[XCRYPTO_SHA256_DIGEST_SIZE];
  ASSERT_EQ(xSha256((const uint8_t *)input, strlen(input), digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA256_DIGEST_SIZE),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, TwoBlocks) {
  const char *input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  uint8_t     digest[XCRYPTO_SHA256_DIGEST_SIZE];
  ASSERT_EQ(xSha256((const uint8_t *)input, strlen(input), digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA256_DIGEST_SIZE),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, MillionAs) {
  /* 1,000,000 repetitions of 'a' */
  std::string input(1000000, 'a');
  uint8_t     digest[XCRYPTO_SHA256_DIGEST_SIZE];
  ASSERT_EQ(xSha256((const uint8_t *)input.data(), input.size(), digest), xErrno_Ok);
  EXPECT_EQ(hex(digest, XCRYPTO_SHA256_DIGEST_SIZE),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

/* ── Streaming API ─────────────────────────────────────── */

TEST(Sha256, StreamingMatchesOneShot) {
  const char *parts[] = {"Hello, ", "World!"};
  const char *full    = "Hello, World!";

  /* One-shot */
  uint8_t digest1[XCRYPTO_SHA256_DIGEST_SIZE];
  ASSERT_EQ(xSha256((const uint8_t *)full, strlen(full), digest1), xErrno_Ok);

  /* Streaming */
  uint8_t    digest2[XCRYPTO_SHA256_DIGEST_SIZE];
  xSha256Ctx ctx;
  ASSERT_EQ(xSha256Init(&ctx), xErrno_Ok);
  ASSERT_EQ(xSha256Update(&ctx, (const uint8_t *)parts[0], strlen(parts[0])), xErrno_Ok);
  ASSERT_EQ(xSha256Update(&ctx, (const uint8_t *)parts[1], strlen(parts[1])), xErrno_Ok);
  ASSERT_EQ(xSha256Final(&ctx, digest2), xErrno_Ok);

  EXPECT_EQ(memcmp(digest1, digest2, XCRYPTO_SHA256_DIGEST_SIZE), 0);
}

/* ── Error handling ────────────────────────────────────── */

TEST(Sha256, NullArgs) {
  uint8_t digest[XCRYPTO_SHA256_DIGEST_SIZE];
  EXPECT_EQ(xSha256(NULL, 0, digest), xErrno_InvalidArg);
  EXPECT_EQ(xSha256((const uint8_t *)"x", 1, NULL), xErrno_InvalidArg);
  EXPECT_EQ(xSha256Init(NULL), xErrno_InvalidArg);
}
