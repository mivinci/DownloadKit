/*
 * base64_test.cpp - Tests for xBase64Encode / xBase64Decode
 */

#include <xbase/base64.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

/* ---- Encode tests ---- */

TEST(Base64, Empty) {
  uint8_t src[]   = {};
  char    buf[8]  = {0};
  size_t  buf_len = sizeof(buf);

  int rc = xBase64Encode(src, 0, buf, &buf_len);
  EXPECT_EQ(rc, 0);
  EXPECT_STREQ(buf, "");
  EXPECT_EQ(buf_len, 0u);
}

TEST(Base64, SingleByte) {
  uint8_t src[]   = {0x41}; /* 'A' */
  char    buf[8]  = {0};
  size_t  buf_len = sizeof(buf);

  int rc = xBase64Encode(src, 1, buf, &buf_len);
  EXPECT_EQ(rc, 0);
  EXPECT_STREQ(buf, "QQ==");
  EXPECT_EQ(buf_len, 4u);
}

TEST(Base64, TwoBytes) {
  uint8_t src[]   = {0x41, 0x42}; /* "AB" */
  char    buf[8]  = {0};
  size_t  buf_len = sizeof(buf);

  int rc = xBase64Encode(src, 2, buf, &buf_len);
  EXPECT_EQ(rc, 0);
  EXPECT_STREQ(buf, "QUI=");
  EXPECT_EQ(buf_len, 4u);
}

TEST(Base64, ThreeBytesExactBlock) {
  uint8_t src[]   = {0x41, 0x42, 0x43}; /* "ABC" */
  char    buf[8]  = {0};
  size_t  buf_len = sizeof(buf);

  int rc = xBase64Encode(src, 3, buf, &buf_len);
  EXPECT_EQ(rc, 0);
  EXPECT_STREQ(buf, "QUJD");
  EXPECT_EQ(buf_len, 4u);
}

TEST(Base64, AllByteValues) {
  uint8_t src[256];
  for (int i = 0; i < 256; i++)
    src[i] = (uint8_t)i;

  char   buf[XBASE64_ENCODE_MAXLEN(256)];
  size_t buf_len = sizeof(buf);

  int rc = xBase64Encode(src, 256, buf, &buf_len);
  EXPECT_EQ(rc, 0);
  /* Known-good prefix: 0x00,0x01,0x02 → "AAEC" */
  EXPECT_EQ(std::string(buf, 4), "AAEC");
}

/* ---- Decode tests ---- */

TEST(Base64, DecodeEmpty) {
  uint8_t buf[8];
  size_t  buf_len = sizeof(buf);

  int rc = xBase64Decode("", 0, buf, &buf_len);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(buf_len, 0u);
}

TEST(Base64, DecodePadded) {
  uint8_t buf[8];
  size_t  buf_len = sizeof(buf);

  /* "QQ==" → {0x41} */
  int rc = xBase64Decode("QQ==", 4, buf, &buf_len);
  EXPECT_EQ(rc, 0);
  ASSERT_EQ(buf_len, 1u);
  EXPECT_EQ(buf[0], 0x41);
}

TEST(Base64, DecodeTwoBytesPadded) {
  uint8_t buf[8];
  size_t  buf_len = sizeof(buf);

  /* "QUI=" → {0x41, 0x42} */
  int rc = xBase64Decode("QUI=", 4, buf, &buf_len);
  EXPECT_EQ(rc, 0);
  ASSERT_EQ(buf_len, 2u);
  EXPECT_EQ(buf[0], 0x41);
  EXPECT_EQ(buf[1], 0x42);
}

TEST(Base64, DecodeThreeBytes) {
  uint8_t buf[8];
  size_t  buf_len = sizeof(buf);

  /* "QUJD" → {0x41, 0x42, 0x43} */
  int rc = xBase64Decode("QUJD", 4, buf, &buf_len);
  EXPECT_EQ(rc, 0);
  ASSERT_EQ(buf_len, 3u);
  EXPECT_EQ(buf[0], 0x41);
  EXPECT_EQ(buf[1], 0x42);
  EXPECT_EQ(buf[2], 0x43);
}

TEST(Base64, DecodeUrlSafe) {
  /* URL-safe alphabet: - instead of +, _ instead of / */
  uint8_t buf[8];
  size_t  buf_len = sizeof(buf);

  /* "AQID" = standard, "AQID" also valid; let me use a case with + and / */
  /* "+/==" → bytes {0xFB, 0xFF} ... no, + is index 62, / is index 63 */
  /* Actually: "+" = 62, "/" = 63 */
  /* Encode {0xFB, 0xFF} = 11111011 11111111 → 111110 111111 111100 → + // 4
   * (with padding) */
  /* Let me just test that - and _ work in decode. */
  /* '-' has same value as '+' (62), '_' same as '/' (63). */
  /* Encoding {0xFB} gives "+/==" in standard, or "-_" in URL-safe...
   * Actually the encoder always uses + and /. Let me just test decode accepts
   * -. */
  /* "AQID" has no + or /. Let me use a known string. */
  /* The encoded form of {0x00, 0xFF, 0xFF} starts with "AP//" */
  int rc = xBase64Decode("AP//", 4, buf, &buf_len);
  EXPECT_EQ(rc, 0);

  /* Now test URL-safe variant: "AP__" should also decode the same. */
  buf_len = sizeof(buf);
  rc      = xBase64Decode("AP__", 4, buf, &buf_len);
  EXPECT_EQ(rc, 0);
  EXPECT_EQ(buf[0], 0x00);
  EXPECT_EQ(buf[1], 0xFF);
  EXPECT_EQ(buf[2], 0xFF);
}

TEST(Base64, DecodeUnpadded) {
  uint8_t buf[8];
  size_t  buf_len = sizeof(buf);

  /* Unpadded: "QQ" (same as "QQ==") → {0x41} */
  int rc = xBase64Decode("QQ", 2, buf, &buf_len);
  EXPECT_EQ(rc, 0);
  ASSERT_EQ(buf_len, 1u);
  EXPECT_EQ(buf[0], 0x41);
}

/* ---- RoundTrip ---- */

TEST(Base64, RoundTrip) {
  uint8_t src[]   = "The quick brown fox jumps over the lazy dog";
  size_t  src_len = sizeof(src) - 1; /* exclude NUL */

  char   encoded[XBASE64_ENCODE_MAXLEN(256)];
  size_t enc_len = sizeof(encoded);
  ASSERT_EQ(xBase64Encode(src, src_len, encoded, &enc_len), 0);

  uint8_t decoded[XBASE64_DECODE_MAXLEN(256)];
  size_t  dec_len = sizeof(decoded);
  ASSERT_EQ(xBase64Decode(encoded, enc_len, decoded, &dec_len), 0);

  EXPECT_EQ(dec_len, src_len);
  EXPECT_TRUE(memcmp(decoded, src, src_len) == 0);
}

/* ---- Error cases ---- */

TEST(Base64, InvalidChar) {
  uint8_t buf[8];
  size_t  buf_len = sizeof(buf);

  EXPECT_EQ(xBase64Decode("AB!C", 4, buf, &buf_len), -1);
  EXPECT_EQ(xBase64Decode("A B", 3, buf, &buf_len),
            -1); /* space invalid unless accepted */
}

TEST(Base64, InvalidLengthMod4Eq1) {
  uint8_t buf[8];
  size_t  buf_len = sizeof(buf);

  /* 1 char mod 4 = 1, invalid even without padding. */
  EXPECT_EQ(xBase64Decode("A", 1, buf, &buf_len), -1);
  /* 5 chars mod 4 = 1, also invalid (unless padded, but then must be multiple
   * of 4). */
  EXPECT_EQ(xBase64Decode("ABCDE", 5, buf, &buf_len), -1);
}

TEST(Base64, PaddingInWrongPosition) {
  uint8_t buf[8];
  size_t  buf_len = sizeof(buf);

  /* Padding must be at end. */
  EXPECT_EQ(xBase64Decode("A=BC", 4, buf, &buf_len), -1);
}

TEST(Base64, EncodeBufferTooSmall) {
  uint8_t src[] = {0x41, 0x42, 0x43};
  char    buf[4]; /* too small: need 4 + 1 */
  size_t  buf_len = sizeof(buf);

  EXPECT_EQ(xBase64Encode(src, 3, buf, &buf_len), -1);
}

TEST(Base64, DecodeBufferTooSmall) {
  uint8_t buf[2]; /* too small: need 3 */
  size_t  buf_len = sizeof(buf);

  EXPECT_EQ(xBase64Decode("QUJD", 4, buf, &buf_len), -1);
}

TEST(Base64, NullInputs) {
  char    buf[8];
  size_t  buf_len = sizeof(buf);
  uint8_t dbuf[8];

  EXPECT_EQ(xBase64Encode(NULL, 1, buf, &buf_len), -1);
  EXPECT_EQ(xBase64Encode(NULL, 0, buf, &buf_len), 0); /* NULL,0 is OK */
  EXPECT_EQ(xBase64Decode(NULL, 0, dbuf, &buf_len), -1);
  EXPECT_EQ(xBase64Decode("QQ==", 4, NULL, &buf_len), -1);
  EXPECT_EQ(xBase64Decode("QQ==", 4, dbuf, NULL), -1);
}
