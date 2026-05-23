/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * utf8_test.cpp - Unit tests for xValidateUtf8
 */

extern "C" {
#include <x/base/utf8.h>
}

#include <gtest/gtest.h>

#include <string>

/* ── Valid sequences ───────────────────────────────────────────────── */

TEST(Utf8ValidateTest, EmptyString) {
  EXPECT_TRUE(xValidateUtf8("", 0));
  EXPECT_TRUE(xValidateUtf8(nullptr, 0));
}

TEST(Utf8ValidateTest, AsciiOnly) {
  const char *s = "Hello, World! 0123456789";
  EXPECT_TRUE(xValidateUtf8(s, strlen(s)));
}

TEST(Utf8ValidateTest, TwoByteChars) {
  /* "café" — é is U+00E9: 0xC3 0xA9 */
  const char *s = "caf\xC3\xA9";
  EXPECT_TRUE(xValidateUtf8(s, strlen(s)));
}

TEST(Utf8ValidateTest, ThreeByteChars) {
  /* "中文" — 中 is U+4E2D: 0xE4 0xB8 0xAD */
  const char *s = "\xE4\xB8\xAD\xE6\x96\x87";
  EXPECT_TRUE(xValidateUtf8(s, 6));
}

TEST(Utf8ValidateTest, FourByteChars) {
  /* U+1F600 (😀): 0xF0 0x9F 0x98 0x80 */
  const char *s = "\xF0\x9F\x98\x80";
  EXPECT_TRUE(xValidateUtf8(s, 4));
}

TEST(Utf8ValidateTest, MixedLengths) {
  /* "a中😀b" */
  const char *s = "a\xE4\xB8\xAD\xF0\x9F\x98\x80" "b";
  EXPECT_TRUE(xValidateUtf8(s, strlen(s)));
}

TEST(Utf8ValidateTest, MaxTwoByte) {
  /* U+07FF: 0xDF 0xBF */
  const char s[] = "\xDF\xBF";
  EXPECT_TRUE(xValidateUtf8(s, 2));
}

TEST(Utf8ValidateTest, MaxThreeByte) {
  /* U+FFFD: 0xEF 0xBF 0xBD */
  const char s[] = "\xEF\xBF\xBD";
  EXPECT_TRUE(xValidateUtf8(s, 3));
}

TEST(Utf8ValidateTest, MaxCodePoint) {
  /* U+10FFFF: 0xF4 0x8F 0xBF 0xBF */
  const char s[] = "\xF4\x8F\xBF\xBF";
  EXPECT_TRUE(xValidateUtf8(s, 4));
}

/* ── Invalid: bad lead bytes ──────────────────────────────────────── */

TEST(Utf8ValidateTest, ContinuationAsLead) {
  /* 0x80 as lead byte */
  const char s[] = "\x80";
  EXPECT_FALSE(xValidateUtf8(s, 1));
}

TEST(Utf8ValidateTest, InvalidLeadFE) {
  const char s[] = "\xFE";
  EXPECT_FALSE(xValidateUtf8(s, 1));
}

TEST(Utf8ValidateTest, InvalidLeadFF) {
  const char s[] = "\xFF";
  EXPECT_FALSE(xValidateUtf8(s, 1));
}

/* ── Invalid: truncated sequences ─────────────────────────────────── */

TEST(Utf8ValidateTest, TruncatedTwoByte) {
  /* 0xC3 alone (needs one more byte) */
  const char s[] = "\xC3";
  EXPECT_FALSE(xValidateUtf8(s, 1));
}

TEST(Utf8ValidateTest, TruncatedThreeByte) {
  /* 0xE4 0xB8 alone (needs one more byte) */
  const char s[] = "\xE4\xB8";
  EXPECT_FALSE(xValidateUtf8(s, 2));
}

TEST(Utf8ValidateTest, TruncatedFourByte) {
  /* 0xF0 0x9F 0x98 alone */
  const char s[] = "\xF0\x9F\x98";
  EXPECT_FALSE(xValidateUtf8(s, 3));
}

/* ── Invalid: bad continuation bytes ──────────────────────────────── */

TEST(Utf8ValidateTest, BadContinuationInTwoByte) {
  /* 0xC3 0x00 — second byte not 10xxxxxx */
  const char s[] = "\xC3\x00";
  EXPECT_FALSE(xValidateUtf8(s, 2));
}

TEST(Utf8ValidateTest, BadContinuationInThreeByte) {
  /* 0xE4 0xB8 0xFF */
  const char s[] = "\xE4\xB8\xFF";
  EXPECT_FALSE(xValidateUtf8(s, 3));
}

TEST(Utf8ValidateTest, BadContinuationInFourByte) {
  /* 0xF0 0x9F 0x98 0x20 */
  const char s[] = "\xF0\x9F\x98\x20";
  EXPECT_FALSE(xValidateUtf8(s, 4));
}

/* ── Invalid: overlong encodings ──────────────────────────────────── */

TEST(Utf8ValidateTest, OverlongTwoByte_C080) {
  /* U+0000 as 2 bytes: 0xC0 0x80 */
  const char s[] = "\xC0\x80";
  EXPECT_FALSE(xValidateUtf8(s, 2));
}

TEST(Utf8ValidateTest, OverlongTwoByte_C1BF) {
  /* U+007F as 2 bytes: 0xC1 0xBF */
  const char s[] = "\xC1\xBF";
  EXPECT_FALSE(xValidateUtf8(s, 2));
}

TEST(Utf8ValidateTest, OverlongThreeByte) {
  /* U+007F as 3 bytes: 0xE0 0x81 0xBF */
  const char s[] = "\xE0\x81\xBF";
  EXPECT_FALSE(xValidateUtf8(s, 3));
}

TEST(Utf8ValidateTest, OverlongFourByte) {
  /* U+07FF as 4 bytes: 0xF0 0x80 0x9F 0xBF */
  const char s[] = "\xF0\x80\x9F\xBF";
  EXPECT_FALSE(xValidateUtf8(s, 4));
}

/* ── Invalid: surrogates (U+D800..U+DFFF) ─────────────────────────── */

TEST(Utf8ValidateTest, SurrogateD800) {
  /* U+D800: 0xED 0xA0 0x80 */
  const char s[] = "\xED\xA0\x80";
  EXPECT_FALSE(xValidateUtf8(s, 3));
}

TEST(Utf8ValidateTest, SurrogateDFFF) {
  /* U+DFFF: 0xED 0xBF 0xBF */
  const char s[] = "\xED\xBF\xBF";
  EXPECT_FALSE(xValidateUtf8(s, 3));
}

TEST(Utf8ValidateTest, SurrogateDB80) {
  /* U+DB80: 0xED 0xAE 0x80 (high surrogate) */
  const char s[] = "\xED\xAE\x80";
  EXPECT_FALSE(xValidateUtf8(s, 3));
}

/* ── Invalid: above U+10FFFF ──────────────────────────────────────── */

TEST(Utf8ValidateTest, AboveMaxF48F) {
  /* U+110000: 0xF4 0x90 0x80 0x80 */
  const char s[] = "\xF4\x90\x80\x80";
  EXPECT_FALSE(xValidateUtf8(s, 4));
}

TEST(Utf8ValidateTest, AboveMaxF5) {
  /* lead byte 0xF5 → always > U+10FFFF */
  const char s[] = "\xF5\x80\x80\x80";
  EXPECT_FALSE(xValidateUtf8(s, 4));
}

/* ── Invalid: embedded in valid data ──────────────────────────────── */

TEST(Utf8ValidateTest, InvalidInMiddle) {
  /* "ab" + bad byte + "cd" */
  const char s[] = "ab\xFE" "cd";
  EXPECT_FALSE(xValidateUtf8(s, 5));
}

TEST(Utf8ValidateTest, InvalidAtEnd) {
  /* valid text then truncated sequence */
  const char s[] = "hello\xC3";
  EXPECT_FALSE(xValidateUtf8(s, 6));
}

/* ── Edge cases ───────────────────────────────────────────────────── */

TEST(Utf8ValidateTest, NullDataNonZeroLen) {
  EXPECT_FALSE(xValidateUtf8(nullptr, 5));
}

TEST(Utf8ValidateTest, JustBelowSurrogate) {
  /* U+D7FF: 0xED 0x9F 0xBF — valid (just below surrogate range) */
  const char s[] = "\xED\x9F\xBF";
  EXPECT_TRUE(xValidateUtf8(s, 3));
}

TEST(Utf8ValidateTest, JustAboveSurrogate) {
  /* U+E000: 0xEE 0x80 0x80 — valid (just above surrogate range) */
  const char s[] = "\xEE\x80\x80";
  EXPECT_TRUE(xValidateUtf8(s, 3));
}

TEST(Utf8ValidateTest, MinTwoByte) {
  /* U+0080: 0xC2 0x80 — smallest valid 2-byte */
  const char s[] = "\xC2\x80";
  EXPECT_TRUE(xValidateUtf8(s, 2));
}

TEST(Utf8ValidateTest, MinThreeByte) {
  /* U+0800: 0xE0 0xA0 0x80 — smallest valid 3-byte */
  const char s[] = "\xE0\xA0\x80";
  EXPECT_TRUE(xValidateUtf8(s, 3));
}

TEST(Utf8ValidateTest, MinFourByte) {
  /* U+10000: 0xF0 0x90 0x80 0x80 — smallest valid 4-byte */
  const char s[] = "\xF0\x90\x80\x80";
  EXPECT_TRUE(xValidateUtf8(s, 4));
}
