/*
 * Copyright 2025 The libx Authors. All rights reserved.
 * Use of this source code is governed by a MIT license that can be
 * found in the LICENSE file.
 *
 * hex_test.cpp - Tests for hex encoding/decoding
 */

#include <gtest/gtest.h>

extern "C" {
#include <x/base/hex.h>
}

#include <cstring>
#include <string>
#include <vector>

/* ── Test vectors ───────────────────────────────────────────── */
struct HexTestCase {
  std::vector<uint8_t> raw;
  std::string          encoded;
};

class HexTest : public ::testing::TestWithParam<HexTestCase> {};

static const HexTestCase kTestCases[] = {
  /* Empty */
  {{}, ""},
  /* Single zero byte */
  {{0x00}, "00"},
  /* Multiple zero bytes */
  {{0x00, 0x00, 0x00}, "000000"},
  /* Simple values */
  {{0x01}, "01"},
  {{0x0a}, "0a"},
  {{0xff}, "ff"},
  /* "Hello" */
  {{0x48, 0x65, 0x6c, 0x6c, 0x6f}, "48656c6c6f"},
  /* All bytes 0x00-0xff */
  {{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff},
   "00112233445566778899aabbccddeeff"},
};

INSTANTIATE_TEST_SUITE_P(Hex, HexTest, ::testing::ValuesIn(kTestCases));

TEST_P(HexTest, Encode) {
  const auto &tc = GetParam();
  char        buf[512];
  size_t      buf_len = sizeof(buf);

  int rc = xHexEncode(tc.raw.data(), tc.raw.size(), buf, &buf_len);
  ASSERT_EQ(rc, 0) << "xHexEncode failed for raw size " << tc.raw.size();
  EXPECT_EQ(buf_len, tc.encoded.size()) << "encoded length mismatch for input " << tc.encoded;
  EXPECT_EQ(std::string(buf, buf_len), tc.encoded) << "encoded content mismatch";
}

TEST_P(HexTest, Decode) {
  const auto &tc = GetParam();
  if (tc.encoded.empty()) {
    /* Decode of "" should succeed with 0 bytes. */
    uint8_t buf[8];
    size_t  buf_len = sizeof(buf);
    int     rc      = xHexDecode("", 0, buf, &buf_len);
    EXPECT_EQ(rc, 0);
    EXPECT_EQ(buf_len, 0u);
    return;
  }

  uint8_t buf[512];
  size_t  buf_len = sizeof(buf);

  int rc = xHexDecode(tc.encoded.c_str(), tc.encoded.size(), buf, &buf_len);
  ASSERT_EQ(rc, 0) << "xHexDecode failed for " << tc.encoded;
  ASSERT_EQ(buf_len, tc.raw.size()) << "decoded length mismatch for " << tc.encoded;
  EXPECT_EQ(std::vector<uint8_t>(buf, buf + buf_len), tc.raw) << "decoded content mismatch";
}

/* ── Round-trip ─────────────────────────────────────────── */
TEST(Hex, RoundTrip) {
  const uint8_t data[]   = "The quick brown fox jumps over the lazy dog";
  size_t        data_len = sizeof(data) - 1; /* exclude NUL terminator */

  char   encoded[512];
  size_t encoded_len = sizeof(encoded);
  ASSERT_EQ(xHexEncode(data, data_len, encoded, &encoded_len), 0);

  uint8_t decoded[512];
  size_t  decoded_len = sizeof(decoded);
  ASSERT_EQ(xHexDecode(encoded, encoded_len, decoded, &decoded_len), 0);

  ASSERT_EQ(decoded_len, data_len);
  EXPECT_EQ(memcmp(data, decoded, data_len), 0);
}

/* ── Decode accepts both upper- and lower-case ────────────── */
TEST(Hex, DecodeCaseInsensitive) {
  uint8_t buf[8];
  size_t  buf_len = sizeof(buf);

  /* Lower-case */
  int rc = xHexDecode("deadbeef", 8, buf, &buf_len);
  ASSERT_EQ(rc, 0);
  uint8_t expected[] = {0xde, 0xad, 0xbe, 0xef};
  EXPECT_EQ(std::vector<uint8_t>(buf, buf + buf_len), std::vector<uint8_t>(expected, expected + 4));

  /* Upper-case */
  buf_len = sizeof(buf);
  rc      = xHexDecode("DEADBEEF", 8, buf, &buf_len);
  ASSERT_EQ(rc, 0);
  EXPECT_EQ(std::vector<uint8_t>(buf, buf + buf_len), std::vector<uint8_t>(expected, expected + 4));

  /* Mixed-case */
  buf_len = sizeof(buf);
  rc      = xHexDecode("DeAdBeEf", 8, buf, &buf_len);
  ASSERT_EQ(rc, 0);
  EXPECT_EQ(std::vector<uint8_t>(buf, buf + buf_len), std::vector<uint8_t>(expected, expected + 4));
}

/* ── Invalid input ──────────────────────────────────────── */
TEST(Hex, InvalidCharacter) {
  uint8_t buf[64];
  size_t  buf_len = sizeof(buf);

  /* 'g' is not a hex digit */
  EXPECT_EQ(xHexDecode("0g", 2, buf, &buf_len), -1);

  /* 'G' is not a hex digit */
  buf_len = sizeof(buf);
  EXPECT_EQ(xHexDecode("0G", 2, buf, &buf_len), -1);

  /* Non-hex ASCII */
  buf_len = sizeof(buf);
  EXPECT_EQ(xHexDecode("0!", 2, buf, &buf_len), -1);
}

/* ── Odd-length input ───────────────────────────────────── */
TEST(Hex, OddLength) {
  uint8_t buf[64];
  size_t  buf_len = sizeof(buf);

  EXPECT_EQ(xHexDecode("0", 1, buf, &buf_len), -1);
  EXPECT_EQ(xHexDecode("abc", 3, buf, &buf_len), -1);
  EXPECT_EQ(xHexDecode("12345", 5, buf, &buf_len), -1);
}

/* ── Buffer too small ───────────────────────────────────── */
TEST(Hex, BufferTooSmall) {
  const uint8_t data[] = {0x01, 0x02, 0x03};
  char          encoded[4]; /* too small: need 6 + 1 */
  size_t        encoded_len = sizeof(encoded);
  EXPECT_EQ(xHexEncode(data, sizeof(data), encoded, &encoded_len), -1);

  uint8_t decoded[1]; /* too small: need 2 */
  size_t  decoded_len = sizeof(decoded);
  EXPECT_EQ(xHexDecode("abcd", 4, decoded, &decoded_len), -1);
}

/* ── Null inputs ───────────────────────────────────────── */
TEST(Hex, NullInputs) {
  size_t len;

  /* xHexEncode: dst is NULL */
  len = 64;
  EXPECT_EQ(xHexEncode((const uint8_t *)"ab", 2, NULL, &len), -1);

  /* xHexEncode: dst_len is NULL */
  char buf[8];
  EXPECT_EQ(xHexEncode((const uint8_t *)"ab", 2, buf, NULL), -1);

  /* xHexEncode: src is NULL with src_len > 0 */
  EXPECT_EQ(xHexEncode(NULL, 1, buf, &len), -1);

  /* xHexDecode: dst is NULL */
  len = 64;
  EXPECT_EQ(xHexDecode("ab", 2, NULL, &len), -1);

  /* xHexDecode: dst_len is NULL */
  uint8_t dbuf[8];
  EXPECT_EQ(xHexDecode("ab", 2, dbuf, NULL), -1);

  /* xHexDecode: src is NULL with src_len > 0 */
  EXPECT_EQ(xHexDecode(NULL, 2, dbuf, &len), -1);
}
